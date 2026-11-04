package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"math"
	"sort"
	"strings"
)

type sectionProgressSummary struct {
	Current             float64  `json:"current"`
	Maximum             float64  `json:"maximum"`
	Percentage          float64  `json:"percentage"`
	ContributingObjects int      `json:"contributing_objects"`
	Status              string   `json:"status"`
	Warnings            []string `json:"warnings"`
}

type sectionProgressConfig struct {
	Mode            string `json:"progress_mode"`
	Depth           string `json:"progress_depth"`
	RelationshipID  string `json:"progress_relationship_field_id"`
	ChildProgressID string `json:"progress_child_field_id"`
}

// attachSectionProgress calculates relationship progress on the server for the
// requested object page. Direct mode retains the old sum-of-valid-children
// behavior. Recursive mode deduplicates leaf objects within each root.
func (a *API) attachSectionProgress(ctx context.Context, sectionID string, items []objectRecord) error {
	if sectionID == "" || len(items) == 0 {
		return nil
	}
	var gameID string
	var raw []byte
	if err := a.db.QueryRowContext(ctx, `SELECT game_id,display_config FROM sections WHERE id=? AND archived=0`, sectionID).Scan(&gameID, &raw); err != nil {
		if err == sql.ErrNoRows {
			return nil
		}
		return err
	}
	var config sectionProgressConfig
	if err := json.Unmarshal(raw, &config); err != nil {
		return err
	}
	if config.Mode != "children" {
		return nil
	}
	if config.Depth == "recursive" {
		return a.attachRecursiveSectionProgress(ctx, gameID, config, items)
	}
	return a.attachDirectSectionProgress(ctx, gameID, config, items)
}

func (a *API) attachDirectSectionProgress(ctx context.Context, gameID string, config sectionProgressConfig, items []objectRecord) error {
	rootIDs := make([]string, 0, len(items))
	summaries := make(map[string]*sectionProgressSummary, len(items))
	for i := range items {
		if !hasField(items[i].Fields, config.RelationshipID) {
			continue
		}
		rootIDs = append(rootIDs, items[i].ID)
		summaries[items[i].ID] = &sectionProgressSummary{Status: "incomplete", Warnings: []string{}}
	}
	if len(rootIDs) == 0 {
		return nil
	}
	placeholders, args := sqlIn(rootIDs)
	query := `SELECT r.object_id,r.referenced_object_id,o.id,o.archived,t.archived,f.field_type,COALESCE(v.value_json,f.default_json)
		FROM object_relationships r
		LEFT JOIN objects o ON o.id=r.referenced_object_id AND o.game_id=r.game_id
		LEFT JOIN object_types t ON t.id=o.object_type_id
		LEFT JOIN field_definitions f ON f.id=? AND f.object_type_id=o.object_type_id AND f.archived=0
		LEFT JOIN field_values v ON v.object_id=o.id AND v.field_id=f.id
		WHERE r.game_id=? AND r.field_id=? AND r.object_id IN (` + placeholders + `)
		ORDER BY r.object_id,r.referenced_object_id`
	allArgs := []any{config.ChildProgressID, gameID, config.RelationshipID}
	allArgs = append(allArgs, args...)
	rows, err := a.db.QueryContext(ctx, query, allArgs...)
	if err != nil {
		return err
	}
	for rows.Next() {
		var rootID, childID string
		var activeObject sql.NullString
		var objectArchived, typeArchived sql.NullInt64
		var fieldType sql.NullString
		var progress []byte
		if err := rows.Scan(&rootID, &childID, &activeObject, &objectArchived, &typeArchived, &fieldType, &progress); err != nil {
			rows.Close()
			return err
		}
		summary := summaries[rootID]
		if summary == nil {
			continue
		}
		if !activeObject.Valid || !objectArchived.Valid || objectArchived.Int64 != 0 || !typeArchived.Valid || typeArchived.Int64 != 0 {
			addProgressWarning(summary, "An archived or unavailable child was excluded.")
			continue
		}
		if !fieldType.Valid || fieldType.String != "progress" {
			addProgressWarning(summary, "A child has no active compatible progress field.")
			continue
		}
		current, maximum, valid := parseProgress(progress)
		if !valid {
			addProgressWarning(summary, "A child has missing or invalid progress values.")
			continue
		}
		summary.Current += current
		summary.Maximum += maximum
		summary.ContributingObjects++
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	if err := rows.Close(); err != nil {
		return err
	}
	for _, summary := range summaries {
		finishProgressSummary(summary)
	}
	for i := range items {
		items[i].SectionProgress = summaries[items[i].ID]
	}
	return nil
}

func (a *API) attachRecursiveSectionProgress(ctx context.Context, gameID string, config sectionProgressConfig, items []objectRecord) error {
	const maxDepth = 64
	const maxNodes = 10000
	rootIDs := make([]string, 0, len(items))
	rootSet := map[string]bool{}
	for i := range items {
		if hasField(items[i].Fields, config.RelationshipID) {
			rootIDs = append(rootIDs, items[i].ID)
			rootSet[items[i].ID] = true
		}
	}
	if len(rootIDs) == 0 {
		return nil
	}
	var ownerType, allowedType string
	var options []byte
	if err := a.db.QueryRowContext(ctx, `SELECT f.object_type_id,f.options_json FROM field_definitions f WHERE f.id=? AND f.archived=0`, config.RelationshipID).Scan(&ownerType, &options); err != nil {
		return err
	}
	var refOptions struct {
		AllowedTypeID string `json:"allowed_type_id"`
	}
	_ = json.Unmarshal(options, &refOptions)
	allowedType = refOptions.AllowedTypeID
	if allowedType == "" || allowedType != ownerType {
		for i := range items {
			if rootSet[items[i].ID] {
				items[i].SectionProgress = &sectionProgressSummary{Status: "error", Warnings: []string{"Recursive progress requires the selected relationship to link objects of the same type."}}
			}
		}
		return nil
	}
	graph := map[string][]string{}
	names := map[string]string{}
	archivedChildren := map[string]bool{}
	hasChildren := map[string]bool{}
	seen := map[string]bool{}
	frontier := uniqueIDs(rootIDs)
	for _, item := range items {
		names[item.ID] = item.Name
		seen[item.ID] = true
	}
	nodeLimitHit, depthLimitHit := false, false
	for depth := 0; len(frontier) > 0; depth++ {
		if depth >= maxDepth {
			depthLimitHit = true
			break
		}
		next := []string{}
		for start := 0; start < len(frontier); start += 400 {
			end := start + 400
			if end > len(frontier) {
				end = len(frontier)
			}
			batch := frontier[start:end]
			placeholders, ids := sqlIn(batch)
			query := `SELECT r.object_id,r.referenced_object_id,o.id,o.name,o.archived,t.archived
				FROM object_relationships r
				LEFT JOIN objects o ON o.id=r.referenced_object_id AND o.game_id=r.game_id
				LEFT JOIN object_types t ON t.id=o.object_type_id
				WHERE r.game_id=? AND r.field_id=? AND r.object_id IN (` + placeholders + `)
				ORDER BY r.object_id,r.referenced_object_id`
			args := []any{gameID, config.RelationshipID}
			args = append(args, ids...)
			rows, err := a.db.QueryContext(ctx, query, args...)
			if err != nil {
				return err
			}
			for rows.Next() {
				var parentID, childID string
				var activeID, name sql.NullString
				var objectArchived, typeArchived sql.NullInt64
				if err := rows.Scan(&parentID, &childID, &activeID, &name, &objectArchived, &typeArchived); err != nil {
					rows.Close()
					return err
				}
				hasChildren[parentID] = true
				if !activeID.Valid || !objectArchived.Valid || objectArchived.Int64 != 0 || !typeArchived.Valid || typeArchived.Int64 != 0 {
					archivedChildren[parentID] = true
					continue
				}
				graph[parentID] = append(graph[parentID], childID)
				names[childID] = name.String
				if !seen[childID] {
					seen[childID] = true
					next = append(next, childID)
					if len(seen) > maxNodes {
						nodeLimitHit = true
						break
					}
				}
			}
			err = rows.Err()
			rows.Close()
			if err != nil {
				return err
			}
			if nodeLimitHit {
				break
			}
		}
		if nodeLimitHit {
			break
		}
		frontier = uniqueIDs(next)
	}
	if nodeLimitHit || depthLimitHit {
		message := "Recursive progress exceeded the safe traversal limit."
		if depthLimitHit {
			message = fmt.Sprintf("Recursive progress exceeded the maximum depth of %d.", maxDepth)
		}
		for i := range items {
			if rootSet[items[i].ID] {
				items[i].SectionProgress = &sectionProgressSummary{Status: "error", Warnings: []string{message}}
			}
		}
		return nil
	}
	progressByLeaf, err := a.loadProgressValues(ctx, gameID, config.ChildProgressID, mapKeys(seen))
	if err != nil {
		return err
	}
	for rootID := range rootSet {
		summary := &sectionProgressSummary{Status: "incomplete", Warnings: []string{}}
		leaves, reachable := map[string]bool{}, map[string]bool{}
		path, visited := map[string]int{}, map[string]bool{}
		var chain []string
		var visit func(string) error
		visit = func(id string) error {
			if start, exists := path[id]; exists {
				cycle := append(append([]string{}, chain[start:]...), id)
				labels := make([]string, len(cycle))
				for i, cycleID := range cycle {
					labels[i] = progressObjectName(names, cycleID)
				}
				return fmt.Errorf("Circular references detected: %s", strings.Join(labels, " -> "))
			}
			if visited[id] {
				return nil
			}
			visited[id] = true
			reachable[id] = true
			if archivedChildren[id] {
				addProgressWarning(summary, progressObjectName(names, id)+" has an archived or unavailable child.")
			}
			children := graph[id]
			if len(children) == 0 && !hasChildren[id] {
				leaves[id] = true
				return nil
			}
			path[id] = len(chain)
			chain = append(chain, id)
			for _, childID := range children {
				if err := visit(childID); err != nil {
					delete(path, id)
					chain = chain[:len(chain)-1]
					return err
				}
			}
			delete(path, id)
			chain = chain[:len(chain)-1]
			return nil
		}
		if err := visit(rootID); err != nil {
			summary.Status = "error"
			summary.Warnings = []string{err.Error()}
			itemsForRoot(items, rootID).SectionProgress = summary
			continue
		}
		for leafID := range leaves {
			value, ok := progressByLeaf[leafID]
			if !ok || !value.valid {
				addProgressWarning(summary, progressObjectName(names, leafID)+" has missing or invalid progress values.")
				continue
			}
			summary.Current += value.current
			summary.Maximum += value.maximum
			summary.ContributingObjects++
		}
		finishProgressSummary(summary)
		itemsForRoot(items, rootID).SectionProgress = summary
	}
	return nil
}

type progressValue struct {
	current, maximum float64
	valid            bool
}

func (a *API) loadProgressValues(ctx context.Context, gameID, fieldID string, objectIDs []string) (map[string]progressValue, error) {
	result := map[string]progressValue{}
	for start := 0; start < len(objectIDs); start += 400 {
		end := start + 400
		if end > len(objectIDs) {
			end = len(objectIDs)
		}
		batch := objectIDs[start:end]
		placeholders, ids := sqlIn(batch)
		query := `SELECT o.id,COALESCE(v.value_json,f.default_json)
			FROM objects o JOIN object_types t ON t.id=o.object_type_id
			LEFT JOIN field_definitions f ON f.id=? AND f.object_type_id=o.object_type_id AND f.archived=0
			LEFT JOIN field_values v ON v.object_id=o.id AND v.field_id=f.id
			WHERE o.game_id=? AND o.archived=0 AND t.archived=0 AND o.id IN (` + placeholders + `)`
		args := []any{fieldID, gameID}
		args = append(args, ids...)
		rows, err := a.db.QueryContext(ctx, query, args...)
		if err != nil {
			return nil, err
		}
		for rows.Next() {
			var id string
			var raw []byte
			if err := rows.Scan(&id, &raw); err != nil {
				rows.Close()
				return nil, err
			}
			current, maximum, valid := parseProgress(raw)
			result[id] = progressValue{current: current, maximum: maximum, valid: valid}
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return nil, err
		}
		if err := rows.Close(); err != nil {
			return nil, err
		}
	}
	return result, nil
}

func parseProgress(raw []byte) (float64, float64, bool) {
	if len(raw) == 0 || string(raw) == "null" {
		return 0, 0, false
	}
	var value struct {
		Current float64 `json:"current"`
		Max     float64 `json:"max"`
	}
	if json.Unmarshal(raw, &value) != nil || math.IsNaN(value.Current) || math.IsInf(value.Current, 0) || math.IsNaN(value.Max) || math.IsInf(value.Max, 0) || value.Max <= 0 {
		return 0, 0, false
	}
	return value.Current, value.Max, true
}

func finishProgressSummary(summary *sectionProgressSummary) {
	if summary == nil {
		return
	}
	if summary.Maximum <= 0 || math.IsNaN(summary.Current) || math.IsInf(summary.Current, 0) || math.IsNaN(summary.Maximum) || math.IsInf(summary.Maximum, 0) {
		summary.Current, summary.Maximum, summary.Percentage = 0, 0, 0
		if len(summary.Warnings) == 0 {
			summary.Warnings = append(summary.Warnings, "No valid progress values were found.")
		}
		summary.Status = "incomplete"
		return
	}
	summary.Percentage = math.Max(0, math.Min(100, summary.Current/summary.Maximum*100))
	if len(summary.Warnings) > 0 {
		summary.Status = "partial"
	} else {
		summary.Status = "complete"
	}
}

func addProgressWarning(summary *sectionProgressSummary, message string) {
	if summary == nil || message == "" {
		return
	}
	for _, existing := range summary.Warnings {
		if existing == message {
			return
		}
	}
	if len(summary.Warnings) < 20 {
		summary.Warnings = append(summary.Warnings, message)
	}
}

func hasField(fields []fieldDefinition, fieldID string) bool {
	for _, field := range fields {
		if field.ID == fieldID && !field.Archived {
			return true
		}
	}
	return false
}

func sqlIn(ids []string) (string, []any) {
	placeholders := strings.TrimSuffix(strings.Repeat("?,", len(ids)), ",")
	args := make([]any, len(ids))
	for i, id := range ids {
		args[i] = id
	}
	return placeholders, args
}

func uniqueIDs(ids []string) []string {
	seen := map[string]bool{}
	result := make([]string, 0, len(ids))
	for _, id := range ids {
		if !seen[id] {
			seen[id] = true
			result = append(result, id)
		}
	}
	sort.Strings(result)
	return result
}

func mapKeys(values map[string]bool) []string {
	result := make([]string, 0, len(values))
	for id := range values {
		result = append(result, id)
	}
	sort.Strings(result)
	return result
}

func progressObjectName(names map[string]string, id string) string {
	if name := names[id]; name != "" {
		return name
	}
	return id
}

func itemsForRoot(items []objectRecord, id string) *objectRecord {
	for i := range items {
		if items[i].ID == id {
			return &items[i]
		}
	}
	return &objectRecord{}
}
