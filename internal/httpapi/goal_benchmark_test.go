package httpapi

import (
	"fmt"
	"net/url"
	"testing"
)

func BenchmarkGoalQueryPage(b *testing.B) {
	e := newTestEnvironment(b)
	u := e.client()
	u.register("goal_query_benchmark")
	game := mapString(decodeMap(b, u.expect("POST", "/api/games", map[string]any{"name": "Goal benchmark"}, 201)), "id")
	tx, err := e.db.Begin()
	if err != nil {
		b.Fatal(err)
	}
	goalStmt, err := tx.Prepare(`INSERT INTO goals(id,game_id,title,description,goal_type,current_value,completed,created_at,updated_at) VALUES(?,?,?,'',?,0,?,?,?)`)
	if err != nil {
		b.Fatal(err)
	}
	taskStmt, err := tx.Prepare(`INSERT INTO goal_tasks(id,goal_id,title,completed,sort_order,created_at) VALUES(?,?,?,?,?,?)`)
	if err != nil {
		b.Fatal(err)
	}
	for i := 0; i < 1000; i++ {
		id := fmt.Sprintf("goal-bench-%04d", i)
		kind := "manual"
		completed := i%4 == 0
		if i%2 == 1 {
			kind = "checklist"
			completed = false
		}
		stamp := fmt.Sprintf("2026-10-%02dT00:00:00Z", i%28+1)
		if _, err := goalStmt.Exec(id, game, fmt.Sprintf("Goal %04d", i), kind, boolInt(completed), stamp, stamp); err != nil {
			b.Fatal(err)
		}
		if kind == "checklist" {
			for task := 0; task < 3; task++ {
				taskID := fmt.Sprintf("task-bench-%04d-%d", i, task)
				taskDone := task < 2 && i%3 == 1
				if _, err := taskStmt.Exec(taskID, id, fmt.Sprintf("Task %d", task), boolInt(taskDone), task, stamp); err != nil {
					b.Fatal(err)
				}
			}
		}
	}
	if err := goalStmt.Close(); err != nil {
		b.Fatal(err)
	}
	if err := taskStmt.Close(); err != nil {
		b.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		b.Fatal(err)
	}
	path := "/api/games/" + url.PathEscape(game) + "/goals?limit=20&offset=0&sort=updated_at&direction=desc"
	status, response := u.call("GET", path, nil)
	if status != 200 {
		b.Fatalf("goal query setup status %d: %s", status, response)
	}
	result := decodeMap(b, response)
	if result["game_total"] != float64(1000) || result["total"] != float64(1000) || len(result["items"].([]any)) != 20 {
		b.Fatalf("goal query setup returned incomplete global totals or page: %#v", result)
	}
	b.ReportAllocs()
	b.ReportMetric(1000, "candidate_goals")
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		status, response := u.call("GET", path, nil)
		if status != 200 {
			b.Fatalf("goal query status %d: %s", status, response)
		}
		b.SetBytes(int64(len(response)))
	}
}
