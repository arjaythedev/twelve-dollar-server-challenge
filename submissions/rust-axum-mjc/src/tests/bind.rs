mod tests {
    use super::parse_addresses;
    use std::net::SocketAddr;

    #[test]
    fn parses_ip_lists_ranges_and_cartesian_ports_in_order() {
        let addresses =
            parse_addresses("127.0.0.[1-2], ::1, 127.0.0.1-127.0.0.2", "3100-3101, 3101").unwrap();
        assert_eq!(
            addresses,
            [
                "127.0.0.1:3100",
                "127.0.0.1:3101",
                "127.0.0.2:3100",
                "127.0.0.2:3101",
                "[::1]:3100",
                "[::1]:3101",
            ]
            .map(|address| address.parse::<SocketAddr>().unwrap())
        );
    }

    #[test]
    fn permits_wildcard_and_single_ephemeral_port() {
        assert_eq!(
            parse_addresses("0.0.0.0", "0").unwrap(),
            ["0.0.0.0:0"].map(|address| address.parse().unwrap())
        );
    }

    #[test]
    fn rejects_empty_reversed_malformed_and_zero_ranges() {
        for (hosts, ports) in [
            ("", "3100"),
            ("127.0.0.[5-1]", "3100"),
            ("127.0.0.2-127.0.0.1", "3100"),
            ("127.0.0.1", "3101-3100"),
            ("127.0.0.1", "0-1"),
            ("127.0.0.1", "0,3100"),
            ("127.0.0.1,,::1", "3100"),
            ("127.0.0.1", "3100,"),
            ("127.0.0.1-::1", "3100"),
        ] {
            assert!(
                parse_addresses(hosts, ports).is_err(),
                "accepted HOST={hosts:?}, PORT={ports:?}"
            );
        }
    }
}
