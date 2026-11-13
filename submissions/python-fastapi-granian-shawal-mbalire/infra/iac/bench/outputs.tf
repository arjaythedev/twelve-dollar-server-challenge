output "server" {
  value = { name = docker_container.server.name, ip = var.server_ip }
}

output "generators" {
  value = [for index, generator in docker_container.generator : {
    name = generator.name
    ip   = cidrhost(var.subnet, var.first_generator_host + index)
    cpus = var.generator_cpu_sets[index]
  }]
}
