.PHONY: audit traceability

audit:
	python3 tools/audit_requirements.py --quick

traceability:
	python3 tools/generate_traceability.py
