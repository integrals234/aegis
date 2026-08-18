# AEGIS participant side (M3/M4/M5, ADR-0020, ADR-0025, ADR-0027). One target
# per layer in configs/architecture_rules.yaml. cpp/participant/strategy
# joined M4 slice 1 under m4-build-wiring (PR #10); cpp/participant/risk joins
# M5 slice 1 under m5-build-wiring (PR #13).
add_subdirectory(feed_handler)
add_subdirectory(book_builder)
add_subdirectory(strategy)
add_subdirectory(risk)
add_subdirectory(oms)
add_subdirectory(portfolio)
add_subdirectory(app)
