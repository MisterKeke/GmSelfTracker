package httpapi

import (
	"fmt"
	"net/url"
	"testing"
)

func BenchmarkObjectQueryPage(b *testing.B) {
	for _, size := range []int{1000, 5000, 10000} {
		b.Run(fmt.Sprintf("objects_%d", size), func(b *testing.B) {
			e := newTestEnvironment(b)
			u := e.client()
			u.register("object_query_benchmark")
			game := mapString(decodeMap(b, u.expect("POST", "/api/games", map[string]any{"name": "Benchmark"}, 201)), "id")
			typ := mapString(decodeMap(b, u.expect("POST", "/api/games/"+game+"/object-types", map[string]any{"name": "Entry"}, 201)), "id")
			field := mapString(decodeMap(b, u.expect("POST", "/api/object-types/"+typ+"/fields", map[string]any{"name": "Score", "field_type": "decimal"}, 201)), "id")
			tx, err := e.db.Begin()
			if err != nil {
				b.Fatal(err)
			}
			objectStmt, err := tx.Prepare(`INSERT INTO objects(id,game_id,object_type_id,name,description,image,archived,created_at,updated_at) VALUES(?,?,?,?,?,'',0,?,?)`)
			if err != nil {
				b.Fatal(err)
			}
			valueStmt, err := tx.Prepare(`INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?)`)
			if err != nil {
				b.Fatal(err)
			}
			for i := 0; i < size; i++ {
				id := fmt.Sprintf("bench-%08d", i)
				stamp := fmt.Sprintf("2026-01-%02dT00:00:00Z", i%28+1)
				if _, err := objectStmt.Exec(id, game, typ, fmt.Sprintf("Object %05d", i), "Representative benchmark record", stamp, stamp); err != nil {
					b.Fatal(err)
				}
				if _, err := valueStmt.Exec(id, typ, field, fmt.Sprintf("%d", i%100), stamp); err != nil {
					b.Fatal(err)
				}
			}
			objectStmt.Close()
			valueStmt.Close()
			if err := tx.Commit(); err != nil {
				b.Fatal(err)
			}

			filters := url.QueryEscape(fmt.Sprintf(`{"mode":"all","conditions":[{"field_id":%q,"operator":"gte","value":50}]}`, field))
			queries := map[string]string{
				"page":               fmt.Sprintf("/api/games/%s/objects?limit=50&offset=500", game),
				"search":             fmt.Sprintf("/api/games/%s/objects?limit=50&offset=0&search=%s", game, url.QueryEscape("Object 00099")),
				"custom_filter_sort": fmt.Sprintf("/api/games/%s/objects?type_id=%s&filters=%s&sort=%s&direction=desc&limit=50&offset=0", game, url.QueryEscape(typ), filters, url.QueryEscape(field)),
			}
			for name, path := range queries {
				status, response := u.call("GET", path, nil)
				if status != 200 {
					b.Fatalf("query setup status %d: %s", status, response)
				}
				result := decodeMap(b, response)
				expectedTotal := size
				expectedPageSize := 50
				if name == "search" {
					expectedTotal, expectedPageSize = 1, 1
				}
				if name == "custom_filter_sort" {
					expectedTotal = size / 2
				}
				if result["total"] != float64(expectedTotal) || len(result["items"].([]any)) != expectedPageSize {
					b.Fatalf("%s over %d objects returned incorrect global results: %#v", name, size, result)
				}
				b.Run(name, func(b *testing.B) {
					b.ReportAllocs()
					b.ReportMetric(float64(size), "dataset_objects")
					b.ResetTimer()
					for i := 0; i < b.N; i++ {
						status, response := u.call("GET", path, nil)
						if status != 200 {
							b.Fatalf("query status %d: %s", status, response)
						}
						b.SetBytes(int64(len(response)))
					}
				})
			}
		})
	}
}
