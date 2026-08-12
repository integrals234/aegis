# AEGIS exchange side (M1, ADR-0012; M3 adds market_data, ADR-0020). One
# target per layer in configs/architecture_rules.yaml; tools/check_architecture.py
# reads the target_link_libraries edges below and rejects any the layer DAG
# does not permit.
add_subdirectory(order_book)
add_subdirectory(sequencer)
add_subdirectory(matching)
add_subdirectory(state)
add_subdirectory(market_data)
add_subdirectory(app)
