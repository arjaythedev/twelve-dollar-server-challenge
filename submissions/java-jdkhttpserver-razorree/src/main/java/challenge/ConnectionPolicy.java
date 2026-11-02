package challenge;

import java.net.InetSocketAddress;
import java.util.HashMap;
import java.util.concurrent.TimeUnit;

/** Transport metadata only: reserve reuse slots before promising another request. */
final class ConnectionPolicy {
    private static final long STALE_NANOS = TimeUnit.SECONDS.toNanos(120);
    private final HashMap<InetSocketAddress, Lease> leases = new HashMap<>();
    private final int capacity;
    private long nextSweep;

    static final class Lease {
        final InetSocketAddress peer;
        long lastUsed;
        int requests;
        boolean active;
        Lease(InetSocketAddress peer) { this.peer = peer; }
    }

    ConnectionPolicy(int capacity) {
        if (capacity < 0) throw new IllegalArgumentException("Negative keep-alive capacity");
        this.capacity = capacity;
    }

    synchronized Lease begin(InetSocketAddress peer) {
        long now = System.nanoTime();
        if (now >= nextSweep) {
            // HttpServer expires idle sockets after 75 s. Leave a conservative
            // margin before reclaiming metadata for clients that disappeared.
            leases.values().removeIf(l -> !l.active && now - l.lastUsed > STALE_NANOS);
            nextSweep = now + TimeUnit.SECONDS.toNanos(5);
        }
        Lease lease = leases.get(peer);
        if (lease == null) {
            if (leases.size() >= capacity) return null;
            lease = new Lease(peer);
            leases.put(peer, lease);
        }
        lease.active = true;
        lease.lastUsed = now;
        return lease;
    }

    synchronized boolean finish(Lease lease, boolean forceClose) {
        if (lease == null) return true;
        lease.active = false;
        lease.lastUsed = System.nanoTime();
        boolean close = forceClose || ++lease.requests >= 32;
        if (close) leases.remove(lease.peer, lease);
        return close;
    }

    synchronized void abort(Lease lease) {
        if (lease != null) leases.remove(lease.peer, lease);
    }
}
