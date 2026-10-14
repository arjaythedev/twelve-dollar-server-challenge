# The Docker setup behind `just bench linux`: one server with the droplet's limits, and load
# generators standing in for the k6 machine, on a bridge network (not loopback).
# bench/linux.py builds the images, applies this with -var, measures, then destroys it.
#
# CPU 0 is the server's vCPU. The generators split CPUs 1-7, and each has its own address, so each
# gets its own ~64k ports to the server's port: 4 generators reach about 256k connections.

provider "docker" {
  host = var.docker_host
}

resource "docker_network" "bench" {
  name = "twelve-net"
  ipam_config {
    subnet = var.subnet
  }
}

resource "docker_container" "server" {
  name        = "twelve-server"
  image       = var.server_image
  command     = length(var.server_command) > 0 ? var.server_command : null
  working_dir = "/app"
  cpu_set     = "0"
  # a 10 ms period, not Docker's 100 ms: at a 15% cap that would stall the server ~85 ms at a time
  cpu_period  = var.server_cpu_quota > 0 ? 10000 : null
  cpu_quota   = var.server_cpu_quota > 0 ? var.server_cpu_quota : null
  memory      = 1792 # MiB: 2 GB minus what an idle droplet's OS uses
  memory_swap = 1792 # no swap
  must_run    = true

  ulimit {
    name = "nofile"
    soft = 65535
    hard = 65535
  }

  networks_advanced {
    name         = docker_network.bench.name
    ipv4_address = var.server_ip
  }
}

resource "docker_container" "generator" {
  count    = length(var.generator_cpu_sets)
  name     = "twelve-loadgen-${count.index}"
  image    = var.generator_image
  command  = ["sleep", "infinity"]
  cpu_set  = var.generator_cpu_sets[count.index]
  must_run = true

  ulimit {
    name = "nofile"
    soft = 1048576
    hard = 1048576
  }

  sysctls = {
    "net.ipv4.ip_local_port_range" = "1024 65535"
    "net.ipv4.tcp_tw_reuse"        = "1"
  }

  networks_advanced {
    name         = docker_network.bench.name
    ipv4_address = cidrhost(var.subnet, var.first_generator_host + count.index)
  }
}
