#!/usr/bin/env ruby

require "yaml"

workflow_path = ARGV.fetch(0, ".github/workflows/publish-image.yml")

def fail_contract(message)
  warn "image publish workflow contract: #{message}"
  exit 1
end

def fetch_key(hash, key, context)
  value = hash[key]
  fail_contract("#{context} is missing #{key}") if value.nil?
  value
end

begin
  workflow = YAML.load_file(workflow_path)
rescue StandardError => e
  fail_contract("invalid YAML: #{e.message}")
end

events = workflow["on"] || workflow[true]
fail_contract("trigger must contain only workflow_dispatch") unless events.is_a?(Hash) && events.keys == ["workflow_dispatch"]

inputs = fetch_key(events["workflow_dispatch"], "inputs", "workflow_dispatch")
%w[release_tag expected_sha].each do |input|
  config = fetch_key(inputs, input, "workflow_dispatch inputs")
  fail_contract("#{input} must be required") unless config["required"] == true
end

concurrency = fetch_key(workflow, "concurrency", "workflow")
fail_contract("publish concurrency must not cancel in progress") unless concurrency["cancel-in-progress"] == false
fail_contract("top-level permissions must be contents: read only") unless workflow["permissions"] == {"contents" => "read"}

jobs = fetch_key(workflow, "jobs", "workflow")
gate = fetch_key(jobs, "release-gate", "jobs")
publish = fetch_key(jobs, "publish", "jobs")

matrix = gate.dig("strategy", "matrix", "include")
architectures = matrix&.map { |entry| entry["arch"] }
fail_contract("release gate must contain exactly amd64 and arm64") unless architectures == %w[amd64 arm64]

gate_steps = fetch_key(gate, "steps", "release-gate")
validation_step = gate_steps.find { |step| step["name"] == "Validate release context" }
validation_run = validation_step&.fetch("run", "")
fail_contract("release gate must execute validate-image-release.sh") unless validation_run&.include?("bash scripts/validate-image-release.sh")

needs = publish["needs"]
fail_contract("publish must depend only on release-gate") unless needs == "release-gate"
expected_permissions = {
  "contents" => "read",
  "packages" => "write",
  "id-token" => "write"
}
fail_contract("publish permissions are not least-privilege release permissions") unless publish["permissions"] == expected_permissions

publish_steps = fetch_key(publish, "steps", "publish")
collision_step = publish_steps.find { |step| step["name"] == "Refuse existing tags" }
collision_run = collision_step&.fetch("run", "")
fail_contract("publish must execute check-image-tags-unused.sh") unless collision_run&.include?("bash scripts/check-image-tags-unused.sh")

build_step = publish_steps.find { |step| step["id"] == "build" }
fail_contract("missing docker build-push step") unless build_step&.fetch("uses", "")&.start_with?("docker/build-push-action@v7")
build_config = build_step["with"]
fail_contract("publish must push") unless build_config["push"] == true
fail_contract("publish must build amd64 and arm64") unless build_config["platforms"] == "linux/amd64,linux/arm64"
tags = build_config["tags"].lines.map(&:strip).reject(&:empty?)
expected_tags = [
  "ghcr.io/ceralive/irl-srt-server:${{ inputs.release_tag }}",
  "ghcr.io/ceralive/irl-srt-server:sha-${{ inputs.expected_sha }}"
]
fail_contract("publish tags must be release and full-SHA tags") unless tags == expected_tags
fail_contract("maximum provenance is required") unless build_config["provenance"] == "mode=max"
fail_contract("SBOM attestation is required") unless build_config["sbom"] == true

sign_step = publish_steps.find { |step| step["name"] == "Sign and verify manifest digest" }
sign_lines = sign_step&.fetch("run", "")&.lines&.map(&:strip)
fail_contract("missing digest signing step") unless sign_lines
fail_contract("Cosign must sign the captured digest") unless sign_lines.include?('cosign sign --yes "${IMAGE_REF}@${DIGEST}"')
verify_index = sign_lines.index("cosign verify \\")
fail_contract("Cosign verification is missing") unless verify_index
verify_lines = sign_lines[verify_index..]
fail_contract("Cosign must verify the captured digest") unless verify_lines.include?('"${IMAGE_REF}@${DIGEST}"')
fail_contract("Cosign must verify GitHub OIDC issuer") unless verify_lines.include?('--certificate-oidc-issuer "https://token.actions.githubusercontent.com" \\')

puts "image publish workflow contract: PASS"
