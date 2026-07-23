#!/usr/bin/env bash
set -euo pipefail

workflow="${1:-.github/workflows/publish-image.yml}"
timeout_seconds="${ACTION_REF_TIMEOUT_SECONDS:-20}"

[[ "${timeout_seconds}" =~ ^[1-9][0-9]*$ ]] || {
	echo "action reference check: timeout must be a positive integer" >&2
	exit 1
}

resolve_ref() {
	local repository="$1"
	local ref="$2"

	if [[ -n "${ACTION_REF_RESOLVER:-}" ]]; then
		timeout "${timeout_seconds}" "${ACTION_REF_RESOLVER}" "${repository}" "${ref}"
		return
	fi

	local encoded_ref
	encoded_ref="$(
		ruby -ruri -e 'puts URI.encode_www_form_component(ARGV.fetch(0))' "${ref}"
	)"
	timeout "${timeout_seconds}" gh api \
		"repos/${repository}/commits/${encoded_ref}" \
		--jq .sha
}

refs_file="$(mktemp)"
trap 'rm -f "${refs_file}"' EXIT

ruby -ryaml -e '
		def each_value(value, &block)
		  case value
		  when Hash
		    value.each do |key, child|
		      yield child if key == "uses"
		      each_value(child, &block)
		    end
		  when Array
		    value.each { |child| each_value(child, &block) }
		  end
		end

		workflow = YAML.load_file(ARGV.fetch(0))
		each_value(workflow) do |action|
		  next if action.start_with?("./", "docker://")

		  match = action.match(%r{\A([^/\s]+/[^/@\s]+)(?:/[^@\s]+)*@([^@\s]+)\z})
		  abort("action reference check: malformed external action #{action}") unless match
		  puts [action, match[1], match[2]].join("\t")
		end
	' "${workflow}" > "${refs_file}"

checked=0
while IFS=$'\t' read -r action repository ref; do
	[[ -n "${action}" ]] || continue
	if resolved_sha="$(resolve_ref "${repository}" "${ref}" 2>&1)"; then
		printf 'resolved %s -> %s\n' "${action}" "${resolved_sha}"
	else
		status=$?
		printf 'action reference check: cannot resolve %s (status %s)\n' \
			"${action}" "${status}" >&2
		printf '%s\n' "${resolved_sha}" >&2
		exit 1
	fi
	checked=$((checked + 1))
done < "${refs_file}"

((checked > 0)) || {
	echo "action reference check: workflow contains no external actions" >&2
	exit 1
}
printf 'action reference check: PASS (%s external references)\n' "${checked}"
