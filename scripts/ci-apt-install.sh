#!/usr/bin/env bash
# Installs Ubuntu packages on CI runners. Usage: scripts/ci-apt-install.sh <package>...
# GitHub's runners sometimes stall in apt: the Azure mirror stops answering, or a download hangs without
# ever hitting apt's own timeouts. Each attempt therefore gets a hard time limit and is retried, instead
# of letting one stalled connection block the job.
set -euo pipefail

echo 'Acquire::Retries "3"; Acquire::http::Timeout "20"; Acquire::https::Timeout "20";' | sudo tee /etc/apt/apt.conf.d/99-ci-timeouts > /dev/null

retry() {
	local limit="$1"
	shift
	for attempt in 1 2 3; do
		if sudo timeout "$limit" "$@"; then
			return 0
		fi
		echo "::warning::'$*' failed or timed out after ${limit}s (attempt $attempt of 3)"
		sleep 5
	done
	return 1
}

retry 120 apt-get update
retry 240 apt-get install -y "$@"
