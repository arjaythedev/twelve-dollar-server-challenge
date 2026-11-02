# Parameters (optional)
# * `arg`: arbitrary arguments to pass to rules (default: none)
# * `env`: used to set `APP_ENV` (default: `dev`)
arg ?=
env ?= dev

# Docker images
# PHP 8.5 and Composer, without the app (local-bench/Dockerfile.tools)
TOOLS_IMAGE = twelve-dollar-php-symfony-tools
TOOLS = docker run --rm --interactive --user $(shell id -u):$(shell id -g) --env HOME=/tmp \
	--volume $(CURDIR):/app --workdir /app
BENCH = bash local-bench/bench.sh

# Executables
COMPOSER = $(TOOLS) $(TOOLS_IMAGE) composer
CONSOLE = $(TOOLS) --env APP_ENV=$(env) --env SQLITE_PATH=/nonexistent/feed.db --env JWT_SECRET=twelve-dollar-challenge \
	$(TOOLS_IMAGE) php bin/console
PHP_CS_FIXER = $(TOOLS) $(TOOLS_IMAGE) php vendor/bin/php-cs-fixer
PHPSTAN = $(TOOLS) $(TOOLS_IMAGE) php vendor/bin/phpstan --memory-limit=256M
RECTOR = $(TOOLS) $(TOOLS_IMAGE) php vendor/bin/rector

# Misc
.DEFAULT_GOAL = help
.PHONY: *

## ——  🍪 The php-symfony-gnugat Makefile  ——————————————————————————————————
## Based on https://github.com/dunglas/symfony-docker
## (arg) denotes the possibility to pass "arg=" parameter to the target
##     this allows to add command and options, example: make composer arg='dump --optimize'
## (env) denotes the possibility to pass "env=" parameter to the target
##     this allows to set APP_ENV environment variable (default: dev), example: make console env='prod' arg='cache:warmup'
help: ## Outputs this help screen
	@grep -E '(^[a-zA-Z0-9\./_-]+:.*?##.*$$)|(^##)' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}{printf "\033[32m%-30s\033[0m %s\n", $$1, $$2}' \
		| sed -e 's/\[32m##/[33m/'

## —— Docker 🐳 ————————————————————————————————————————————————————————————————
docker: ## Runs Docker (arg, eg `arg='images'`)
	@docker $(arg)

docker-init: ## Builds the tools image (PHP 8.5 and Composer)
	@docker build --pull --file local-bench/Dockerfile.tools --tag $(TOOLS_IMAGE) local-bench

docker-bash: ## Opens a (bash) shell in the tools container
	@$(TOOLS) --tty $(TOOLS_IMAGE) bash

## —— PHP 🐘 ———————————————————————————————————————————————————————————————————
composer: ## Runs Composer (arg, eg `arg='outdated'`)
	@$(COMPOSER) $(arg)

composer-install: ## Install dependencies (arg, eg `arg='--no-dev'`)
	@$(COMPOSER) install --optimize-autoloader $(arg)

composer-update: ## Updates dependencies (arg, eg `arg='--no-dev'`)
	@$(COMPOSER) update --optimize-autoloader $(arg)

composer-dump: ## Dumps autoloader (arg, eg `arg='--classmap-authoritative'`)
	@$(COMPOSER) dump-autoload --optimize --strict-psr --strict-ambiguous $(arg)

console: ## Runs Symfony Console (arg, env, eg `arg='debug:router' env='prod'`)
	@$(CONSOLE) $(arg)

cs-check: ## Checks CS with PHP-CS-Fixer (arg, eg `arg='src/Controller'`)
	@$(PHP_CS_FIXER) check --verbose $(arg)

cs-fix: ## Fixes CS with PHP-CS-Fixer
	@$(PHP_CS_FIXER) fix --verbose $(arg)

phpstan: ## Runs phpstan (arg, eg `arg='clear-result-cache'`)
	@$(PHPSTAN) $(arg)

phpstan-analyze: ## Static Analysis with phpstan (arg, eg `arg='src/Controller'`)
	@$(PHPSTAN) analyze $(arg)

rector-fix: ## Automated refactorings with Rector (arg, eg `arg='--clear-cache'`)
	@$(RECTOR) $(arg)

rector-check: ## Refactoring checks with Rector
	@$(RECTOR) process --dry-run

## —— Bench 🏋️ ——————————————————————————————————————————————————————————————————
bench-build: ## Builds the app image (runs install.sh and build.sh, like the box)
	@$(BENCH) build

bench-test: ## Runs the challenge's test suite in the app image (what CI runs), run bench-build first
	@$(BENCH) test

bench-start: ## Starts the server on http://127.0.0.1:3000 (1 CPU, 2 GB) on a fresh database
	@$(BENCH) start

bench-load: ## Runs the k6 load test against the server (arg, eg `arg='2000 my-label'`)
	@$(BENCH) load $(arg)

bench-stop: ## Stops the server
	@$(BENCH) stop

## —— App 📱 ———————————————————————————————————————————————————————————————————
app-init: ## First install / resetting (tools image, Composer dependencies)
	@echo ''
	@echo '  // Building the tools image...'
	@$(MAKE) docker-init
	@echo ''
	@echo '  // Installing Composer dependencies...'
	@$(MAKE) composer-install
	@echo ''
	@echo '  [OK] php-symfony-gnugat initialized'

app-clear: ## Clears the Symfony cache (env, eg `env='prod'`)
	@$(CONSOLE) cache:clear

app-qa: ## Runs full QA pipeline (composer-dump, cs-check, phpstan-analyze, rector-check, bench-build, bench-test)
	@echo ''
	@echo '  // Running composer dump...'
	@$(MAKE) composer-dump
	@echo ''
	@echo '  // Running PHP CS Fixer...'
	@$(MAKE) cs-check
	@echo ''
	@echo '  // Running PHPStan...'
	@$(MAKE) phpstan-analyze
	@echo ''
	@echo '  // Running Rector...'
	@$(MAKE) rector-check
	@echo ''
	@echo '  // Building the app image...'
	@$(MAKE) bench-build
	@echo ''
	@echo '  // Running the test suite...'
	@$(MAKE) bench-test
	@echo ''
	@echo '  [OK] QA done'
