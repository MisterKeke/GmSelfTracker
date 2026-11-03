.PHONY: dev build test fmt vet help

dev:
	go run ./cmd/server

build:
	go build -o gametracker.exe ./cmd/server

test:
	go test ./...

fmt:
	go fmt ./...

vet:
	go vet ./...

help:
	@echo "make dev   Start the local server at http://localhost:8080"
	@echo "make build Build the Go server"
	@echo "make test  Run automated tests"
	@echo "make fmt   Format Go source"
	@echo "make vet   Run go vet"
