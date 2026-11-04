.PHONY: dev build test test-js fmt vet help

dev:
	go run ./cmd/server

build:
	go build -o gametracker.exe ./cmd/server

test:
	go test ./...

# Optional frontend regression checks; the application itself only needs Go.
test-js:
	node --test web/js/tests/*.test.mjs

fmt:
	go fmt ./...

vet:
	go vet ./...

help:
	@echo "make dev   Start the local server at http://localhost:8080"
	@echo "make build Build the Go server"
	@echo "make test  Run automated tests"
	@echo "make test-js  Run optional frontend tests (Node.js 22+)"
	@echo "make fmt   Format Go source"
	@echo "make vet   Run go vet"
