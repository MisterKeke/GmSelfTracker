package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"gametracker/internal/database"
	"gametracker/internal/httpapi"
)

func main() {
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	host := env("HOST", "127.0.0.1")
	port := env("PORT", "8080")
	dbPath := env("DB_PATH", "./data/gametracker.db")
	uploadDir := env("UPLOAD_DIR", "./data/uploads")
	production := env("APP_ENV", "development") == "production"

	startupCtx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	db, err := database.Open(startupCtx, dbPath)
	cancel()
	if err != nil {
		logger.Error("database startup failed", "error", err)
		os.Exit(1)
	}
	defer db.Close()
	if err := os.MkdirAll(uploadDir, 0o755); err != nil {
		logger.Error("upload directory startup failed", "error", err)
		os.Exit(1)
	}

	server := &http.Server{
		Addr:              host + ":" + port,
		Handler:           httpapi.New(db, uploadDir, production, logger),
		ReadHeaderTimeout: 5 * time.Second,
		IdleTimeout:       60 * time.Second,
	}
	stopped := make(chan os.Signal, 1)
	signal.Notify(stopped, os.Interrupt, syscall.SIGTERM)
	go func() {
		logger.Info("GameTracker is ready", "url", "http://localhost:"+port)
		if err := server.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("server stopped", "error", err)
			os.Exit(1)
		}
	}()
	<-stopped
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := server.Shutdown(ctx); err != nil {
		logger.Error("server shutdown failed", "error", err)
	}
}

func env(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}
