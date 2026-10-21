# Scripted sequences against a FakeDevice: the state machine, slot ownership
# and publish rules of src/iris without hardware (docs/architecture.md §9).
session_test = executable(
  'session-test',
  ['unit/session-test.cc', 'unit/fake_device.cc'],
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('session', session_test)

# Hardware probe, built but not registered as a meson test: it needs a
# decode-order Iris node and arguments. Proves by content that picture k
# lands in the k-th queued client DMA-BUF; the acceptance test
# docs/architecture.md §9 requires for the Import policy.
executable(
  'iris-import-probe',
  'iris-import-probe.cc',
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])

# Hardware probe, built but not registered as a meson test: it needs the
# encoder node. Answers whether the loop-filter controls exist and whether a
# deblocking request the firmware cannot honour is refused, which is the §10
# invariant 13 gate a scripted node cannot give a verdict on.
executable(
  'iris-encode-control-probe',
  'iris-encode-control-probe.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])

# Parameter-set bitstream round trips for the HEVC translator.
hevc_parameter_sets_test = executable(
  'hevc-parameter-sets-test',
  'hevc-parameter-sets-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('hevc-parameter-sets', hevc_parameter_sets_test)

# VA encode parameters -> V4L2 controls, both codecs, no hardware.
encode_params_test = executable(
  'encode-params-test',
  'encode-params-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('encode-params', encode_params_test)

# The encoder session's control contract on a scripted Iris encoder node:
# which V4L2 writes a VA deblocking request produces, and what happens on a
# kernel that has no loop-filter controls at all.
encode_session_test = executable(
  'encode-session-test',
  'unit/encode-session-test.cc',
  dependencies: [iris_core_dep],
  cpp_args: ['-Wall', '-Wextra', '-std=c++20'])
test('encode-session', encode_session_test)

# Hardware-independent regression guards for the browser-facing queue and
# stable-export invariants. The integration probe above remains opt-in because
# hosts without /dev/video nodes cannot execute it.
#
# run-static-checks.sh is the same entry point CI uses, so `meson test` and CI
# cannot drift apart. The individual checks stay registered as well to keep
# failures attributable without reading the aggregate output.
test('static-checks', find_program('run-static-checks.sh'),
     args: [meson.project_source_root()])
test('iris-source-invariants', find_program('iris-source-invariants.sh'),
     args: [meson.project_source_root()])
test('iris-env-doc-check', find_program('iris-env-doc-check.sh'),
     args: [meson.project_source_root()])
test('iris-codec-matrix-check', find_program('iris-codec-matrix-check.sh'),
     args: [meson.project_source_root()])
test('iris-experiment-guard-self-test', find_program('iris-experiment-guard-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-eos-check-self-test', find_program('iris-eos-check-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-deploy-path-check', find_program('iris-deploy-path-check.sh'),
     args: [meson.project_source_root()])
test('iris-setup-path-check', find_program('iris-setup-path-check.sh'),
     args: [meson.project_source_root()])
test('iris-packaging-check', find_program('iris-packaging-check.sh'),
     args: [meson.project_source_root()])
test('iris-install-manifest-check', find_program('iris-install-manifest-check.sh'),
     args: [meson.project_source_root()])
test('provenance-self-test', find_program('provenance-self-test.sh'),
     args: [meson.project_source_root()])
test('iris-browser-launcher-check', find_program('iris-browser-launcher-check.sh'),
     args: [meson.project_source_root()])
