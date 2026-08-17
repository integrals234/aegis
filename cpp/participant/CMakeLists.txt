# AEGIS participant side (M3/M4, ADR-0020, ADR-0025). One target per layer in
# configs/architecture_rules.yaml. cpp/participant/strategy joined M4 slice 1
# under m4-build-wiring (PR #10); cpp/participant/risk stays a
# .gitkeep-only directory until M5.
add_subdirectory(feed_handler)
add_subdirectory(book_builder)
add_subdirectory(strategy)
add_subdirectory(oms)
add_subdirectory(portfolio)
add_subdirectory(app)
