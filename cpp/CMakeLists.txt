# AEGIS C++ layers (AEGIS-004, AEGIS-227).
#
# One target per layer in configs/architecture_rules.yaml, named
# aegis_<layer without its language prefix>. tools/check_architecture.py reads
# the target_link_libraries edges below and rejects any that the layer DAG does
# not permit, so the build graph and the architecture cannot drift apart.
#
# Layers that belong to later milestones (exchange, participant, replay, memory,
# queues, statistics) are deliberately absent: the roadmap places them in M1-M8,
# and an empty placeholder target would let their rules pass vacuously.

add_subdirectory(common)
