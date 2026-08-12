# AEGIS participant side (M3, ADR-0020). One target per layer in
# configs/architecture_rules.yaml. cpp/participant/strategy stays absent
# until M4 (configs/architecture_rules.yaml dates it there); cpp/participant/risk
# stays a .gitkeep-only directory until M5.
add_subdirectory(feed_handler)
add_subdirectory(book_builder)
add_subdirectory(oms)
add_subdirectory(portfolio)
add_subdirectory(app)
