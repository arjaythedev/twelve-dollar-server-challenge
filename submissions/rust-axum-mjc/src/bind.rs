use std::{
    collections::HashSet,
    io,
    net::{IpAddr, Ipv4Addr, SocketAddr},
};

const MAX_BIND_ADDRESSES: usize = 65_536;

/// Expands the HOST and PORT settings into unique socket addresses, preserving input order.
pub fn parse_addresses(hosts: &str, ports: &str) -> io::Result<Vec<SocketAddr>> {
    let hosts = parse_hosts(hosts)?;
    let ports = parse_ports(ports)?;
    let capacity = hosts
        .len()
        .checked_mul(ports.len())
        .ok_or_else(invalid_binding)?;
    if capacity > MAX_BIND_ADDRESSES {
        return Err(invalid_binding());
    }
    let mut addresses = Vec::new();
    addresses
        .try_reserve_exact(capacity)
        .map_err(|_| invalid_binding())?;
    for host in hosts {
        for &port in &ports {
            addresses.push(SocketAddr::new(host, port));
        }
    }
    Ok(addresses)
}

fn parse_hosts(value: &str) -> io::Result<Vec<IpAddr>> {
    let mut hosts = Vec::new();
    let mut seen = HashSet::new();
    for entry in value.split(',').map(str::trim) {
        if entry.is_empty() {
            return Err(invalid_binding());
        }
        if let Some((prefix, range)) = entry.split_once(".[") {
            let range = range.strip_suffix(']').ok_or_else(invalid_binding)?;
            let (first, last) = range.split_once('-').ok_or_else(invalid_binding)?;
            let first: u8 = first.parse().map_err(|_| invalid_binding())?;
            let last: u8 = last.parse().map_err(|_| invalid_binding())?;
            if first > last {
                return Err(invalid_binding());
            }
            let prefix: Ipv4Addr = format!("{prefix}.0")
                .parse()
                .map_err(|_| invalid_binding())?;
            let octets = prefix.octets();
            for last_octet in first..=last {
                let host = IpAddr::V4(Ipv4Addr::new(octets[0], octets[1], octets[2], last_octet));
                if seen.insert(host) {
                    hosts.push(host);
                }
            }
        } else if let Some((first, last)) = entry.split_once('-') {
            let first: Ipv4Addr = first.parse().map_err(|_| invalid_binding())?;
            let last: Ipv4Addr = last.parse().map_err(|_| invalid_binding())?;
            let first = u32::from(first);
            let last = u32::from(last);
            if first > last {
                return Err(invalid_binding());
            }
            let count = u64::from(last) - u64::from(first) + 1;
            if count > MAX_BIND_ADDRESSES as u64
                || hosts.len() + count as usize > MAX_BIND_ADDRESSES
            {
                return Err(invalid_binding());
            }
            let count = count as usize;
            hosts.try_reserve(count).map_err(|_| invalid_binding())?;
            for value in first..=last {
                let host = IpAddr::V4(Ipv4Addr::from(value));
                if seen.insert(host) {
                    hosts.push(host);
                }
            }
        } else {
            let host: IpAddr = entry.parse().map_err(|_| invalid_binding())?;
            if seen.insert(host) {
                hosts.push(host);
            }
        }
        if hosts.len() > MAX_BIND_ADDRESSES {
            return Err(invalid_binding());
        }
    }
    Ok(hosts)
}

fn parse_ports(value: &str) -> io::Result<Vec<u16>> {
    let allow_zero = value.trim() == "0";
    let mut ports = Vec::new();
    let mut seen = HashSet::new();
    for entry in value.split(',').map(str::trim) {
        if entry.is_empty() {
            return Err(invalid_binding());
        }
        if let Some((first, last)) = entry.split_once('-') {
            let first: u16 = first.parse().map_err(|_| invalid_binding())?;
            let last: u16 = last.parse().map_err(|_| invalid_binding())?;
            if first == 0 || first > last {
                return Err(invalid_binding());
            }
            ports
                .try_reserve((last - first) as usize + 1)
                .map_err(|_| invalid_binding())?;
            for port in first..=last {
                if seen.insert(port) {
                    ports.push(port);
                }
            }
            if ports.len() > MAX_BIND_ADDRESSES {
                return Err(invalid_binding());
            }
        } else {
            let port: u16 = entry.parse().map_err(|_| invalid_binding())?;
            if port == 0 && !allow_zero {
                return Err(invalid_binding());
            }
            if seen.insert(port) {
                ports.push(port);
            }
            if ports.len() > MAX_BIND_ADDRESSES {
                return Err(invalid_binding());
            }
        }
    }
    Ok(ports)
}

fn invalid_binding() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidInput,
        "HOST and PORT must contain valid addresses and ascending ranges",
    )
}

#[cfg(test)]
include!("tests/bind.rs");
