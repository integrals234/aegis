# Benchmark Methodology Policy

Never write a target number into the CV before measuring it.

Every benchmark must record:
- CPU model, RAM, OS/kernel, virtualization/container status;
- compiler/version and optimization flags;
- power governor/turbo/affinity configuration;
- build type, logging, sanitizers and assertions;
- warm-up procedure;
- message mix and number of events;
- active instruments, levels, orders and fill distribution;
- allocation count;
- median, p95, p99, p99.9, maximum latency;
- throughput and CPU utilization;
- raw result artifact and reproducible command.

Required workload example (adjust only with documentation):
- 45% add,
- 30% cancel,
- 10% modify,
- 10% marketable orders,
- 5% large orders crossing multiple levels.

Rules:
1. Isolated in-process matching latency is not called end-to-end latency.
2. WSL, VM and container results are labeled as such.
3. Mean latency alone is forbidden in CV claims.
4. Optimized and baseline implementations must produce equivalent canonical outputs.
5. A regression threshold requires repeated-run statistics, not one lucky run.
