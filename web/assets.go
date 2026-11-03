package web

import (
	"embed"
	"io/fs"
)

//go:embed index.html css js
var assets embed.FS

func Files() (fs.FS, error) { return fs.Sub(assets, ".") }
