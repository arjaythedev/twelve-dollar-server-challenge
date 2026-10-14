variable "docker_host" {
  description = "Docker endpoint of the bench VM, e.g. unix:///Users/me/.colima/bench/docker.sock"
  type        = string
}

variable "server_image" {
  description = "The submission under test (built by bench/linux.py)"
  type        = string
}

variable "server_command" {
  description = "Overrides the image's command (used for extra Granian flags); empty keeps it"
  type        = list(string)
  default     = []
}

variable "server_cpu_quota" {
  description = "Server CPU time per 10 ms period, in microseconds; 0 leaves it uncapped"
  type        = number
  default     = 0
}

variable "generator_image" {
  type    = string
  default = "twelve-loadgen"
}

variable "generator_cpu_sets" {
  description = "One generator container per entry, pinned to these CPUs"
  type        = list(string)
  default     = ["1-2", "3-4", "5-6", "7"]
}

variable "subnet" {
  type    = string
  default = "172.30.0.0/24"
}

variable "server_ip" {
  type    = string
  default = "172.30.0.10"
}

variable "first_generator_host" {
  description = "Host number of the first generator in the subnet; the rest follow"
  type        = number
  default     = 20
}
