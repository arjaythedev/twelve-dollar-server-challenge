"""Print the five-minute standings from the archived four-generator comparisons."""

import argparse
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent
PRIMARY = 'single-queue-split-polling-tap-external-txring4096'
PROFILES = {
    'primary': ('ring4096-', PRIMARY),
    'large-hash': ('large512k-', PRIMARY),
    'rps': ('rps4096-', PRIMARY + '-ordinary-rps'),
    'memory': ('ram4096-', PRIMARY + '-private-passive-tcp-memory'),
}
CANDIDATES = {
    'public-pcache': ('willwang-public-pcache', 'Willwang, C with public PCACHE2 (b4ed5678)'),
    'grouped': ('willwang-batched-epoll-registration', 'Willwang, C with grouped registrations'),
    'grouped-sqlite': ('willwang-grouped-sqlite-sibling-swap', 'Willwang, C with grouped registrations and SQLite sibling-key restoration'),
    'explicit': ('willwang-explicit-slot-registration', 'Willwang, C with explicit slots and stock SQLite'),
    'stock-index-seek': ('willwang-stock-index-seek', 'Willwang, C with stock SQLite end-of-leaf seeks'),
    'explicit-pages': ('willwang-explicit-slot-connection-pages', 'Willwang, C with explicit slots and huge pages'),
    'leaf-cursor': ('willwang-consumed-sqlite-leaf', 'Willwang, C experimental leaf jumps and SQLite patch'),
    'explicit-sqlite': ('willwang-explicit-sqlite-sibling-swap', 'Willwang, C experimental SQLite sibling-key restoration'),
}
PCACHE_ELF = 'b4ed5678e37895d8323ecacf67707e2bdd08a4d499efcbfd7ee2e5b1a8362bdb'
PEERS = {
    'kensac': 'Kensac, Rust',
    'mojo': 'Oh-meh, Mojo',
    'rkkautsar': 'Rkkautsar, Rust',
    'swift': 'Akbashev, Swift',
    'pavlyhalim': 'Pavlyhalim, C',
}


def eligible(trial, fixture, profile, digest, include_unlocked, hold=300, prefix=None):
    default_prefix, topology = PROFILES[profile]
    prefix = prefix or default_prefix
    return (
        trial['trial'].startswith(prefix)
        and fixture['topology'] == topology
        and (profile != 'large-hash' or (fixture['server_ehash_requested'] == 524288
                                         and int(fixture['server_ehash_actual']) == 524288))
        and trial['server_sha256'] == digest
        and trial['hold_s'] == hold
        and trial['stock_schedule_complete']
        and trial['scored_healthy_environment']
        and not trial['target_network_flags']
        and (include_unlocked or trial['host_observations'].get('component_test_lock_covered_score'))
    )


def verdict(trial):
    if not trial['stock_pass']:
        return 'miss'
    if trial['qualified']:
        return 'pass'
    full = trial.get('hold_full_connection_ticks')
    expected = trial.get('hold_expected_connection_ticks')
    if full is not None and expected is not None and full < expected:
        return f'stock pass, {full}/{expected} full checks'
    return 'stock pass, local qualification missed'


def row(label, official, trials, users):
    by_users = {}
    for trial in trials:
        by_users.setdefault(trial['users'], []).append(trial['qualified'])
    high = max((count for count, passes in by_users.items() if any(passes)), default=0)
    passed_all = max((count for count, passes in by_users.items()
                      if len(passes) >= 2 and all(passes)), default=0)
    best = '—' if trials else 'No matched run'
    if high:
        passes = by_users[high]
        best = f'{high:,} ({sum(passes)}/{len(passes)})'
    stock_high = max((trial['users'] for trial in trials if trial['stock_pass']), default=0)
    stock_best = '—' if trials else 'No matched run'
    if stock_high:
        attempts = [trial['stock_pass'] for trial in trials if trial['users'] == stock_high]
        stock_best = f'{stock_high:,} ({sum(attempts)}/{len(attempts)})'
    above = [trial['users'] for trial in trials if trial['users'] > high and not trial['qualified']]
    first_miss = f'{min(above):,}' if above else '—'
    same_load = []
    for trial in trials:
        if trial['users'] == users:
            same_load.append(f"{trial['p99_ms']:,.0f} ms ({verdict(trial)})")
    published = f'{official:,}' if official else 'Unscored'
    repeated = f'{passed_all:,}' if passed_all else '—'
    matched = ', '.join(same_load) or 'No eligible measurement at this load'
    return f"| {label} | {published} | {stock_best} | {best} | {repeated} | {first_miss} | {matched} |"


def pcache_trials(hold):
    report = json.loads((HERE / 'public-pcache-capacity.json').read_text())
    return [
        {'users': trial['target'], 'stock_pass': trial['stock']['pass'],
         'qualified': trial['verified_qualified_pass'], 'p99_ms': trial['stock']['p99_ms']}
        for trial in report['runs']
        if trial['state'] == 'final' and trial['eligible_head_to_head']
        and trial['identity']['sha256'] == PCACHE_ELF and trial['hold_s'] == hold
    ]


def pcache_diagnostics(users, hold):
    report = json.loads((HERE / 'public-pcache-capacity.json').read_text())
    for trial in report['runs']:
        if (trial['state'] == 'final' and not trial['eligible_head_to_head']
                and trial['identity']['sha256'] == PCACHE_ELF
                and trial['target'] == users and trial['hold_s'] == hold):
            occupancy = trial['occupancy']
            stock = trial['stock']
            print(f"\nSelected ELF diagnostic at {users:,} users "
                  f"({occupancy['each_shard_connected']}/{occupancy['expected']} full checks), "
                  f"{stock['p95_ms']:,.0f}/{stock['p99_ms']:,.0f} ms p95/p99, "
                  f"{stock['failed']:,} errors. Excluded from matched standings.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--at', type=int, default=300000, help='Simultaneous users for the p99 column.')
    parser.add_argument('--hold', type=int, choices=(120, 300), default=300,
                        help='Stock hold duration in seconds (120 for search, 300 for confirmation).')
    parser.add_argument('--profile', choices=PROFILES,
                        help='Network fixture (default: large-hash for PCACHE2, primary otherwise).')
    parser.add_argument('--candidate', choices=CANDIDATES, default='public-pcache',
                        help='Local C executable to compare with the five scored competitors.')
    parser.add_argument('--include-unlocked', action='store_true',
                        help='Also include earlier trials where component work was allowed on the host.')
    args = parser.parse_args()
    if args.profile is None:
        args.profile = 'large-hash' if args.candidate == 'public-pcache' else 'primary'
    entries = {entry['entry']: entry for entry in json.loads((HERE / 'entries.json').read_text())}
    results = json.loads((HERE / 'local-results.json').read_text())
    duration = 'five-minute' if args.hold == 300 else 'two-minute'
    print(f'| Entry | Published users | Highest stock {duration} pass | Highest locally qualified pass | Highest all-qualified repeat (2+ runs) | Next tested local miss | Matched p99 at {args.at:,} |')
    print('| --- | ---: | --- | --- | ---: | ---: | --- |')
    for name, label in [CANDIDATES[args.candidate], *PEERS.items()]:
        if name == 'willwang-public-pcache':
            trials = pcache_trials(args.hold) if args.profile == 'large-hash' else []
            print(row(label, None, trials, args.at))
            continue
        entry = entries[name]
        trials = []
        for trial in results['trials']:
            if trial['entry'] != name:
                continue
            fixture = results['fixtures'][trial['fixture'] - 1]
            if eligible(trial, fixture, args.profile, entry['local_server_sha256'], args.include_unlocked, args.hold):
                trials.append(trial)
        print(row(label, entry.get('official_users'), trials, args.at))

    if args.profile == 'large-hash':
        if args.candidate == 'public-pcache':
            pcache_diagnostics(args.at, args.hold)
        labels = {**PEERS, 'willwang-0142ba2': 'Willwang, original C entry'}
        controls = []
        for trial in results['trials']:
            name = trial['entry']
            if name not in labels:
                continue
            fixture = results['fixtures'][trial['fixture'] - 1]
            if eligible(trial, fixture, args.profile, entries[name]['local_server_sha256'],
                        args.include_unlocked, args.hold, prefix='hash512k-'):
                controls.append(f"| {labels[name]} | {trial['users']:,} | {trial['p99_ms']:,.0f} ms | {verdict(trial)} |")
        if controls:
            print("\nSeparate calibration runs with officially scored builds\n")
            print("| Entry | Users | p99 | Local verdict |")
            print("| --- | ---: | ---: | --- |")
            print("\n".join(controls))


if __name__ == '__main__':
    main()
