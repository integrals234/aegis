# AEGIS exchange side (M1, ADR-0012). One target per layer in
# configs/architecture_rules.yaml; tools/check_architecture.py reads the
# target_link_libraries edges below and rejects any the layer DAG does not
# permit. cpp/exchange/market_data stays a .gitkeep-only directory until M3.
add_subdirectory(order_book)
add_subdirectory(sequencer)
add_subdirectory(matching)
add_subdirectory(state)
add_subdirectory(app)
