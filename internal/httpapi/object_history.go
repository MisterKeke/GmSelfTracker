package httpapi

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"time"
)

type objectHistoryChange struct {
	FieldID         string          `json:"field_id,omitempty"`
	FieldName       string          `json:"field_name"`
	FieldType       string          `json:"field_type,omitempty"`
	BeforeFieldType string          `json:"before_field_type,omitempty"`
	Before          json.RawMessage `json:"before,omitempty"`
	After           json.RawMessage `json:"after,omitempty"`
}

type objectHistoryField struct {
	FieldID   string `json:"field_id"`
	FieldName string `json:"field_name"`
	FieldType string `json:"field_type"`
}

type objectHistoryItem struct {
	ID         string                `json:"id"`
	ObjectName string                `json:"object_name"`
	EventType  string                `json:"event_type"`
	Source     string                `json:"source"`
	CreatedAt  string                `json:"created_at"`
	Changes    []objectHistoryChange `json:"changes"`
}

func historyTime(value time.Time) string {
	return value.UTC().Format("2006-01-02T15:04:05.000000000Z")
}

func (a *API) writeObjectHistory(ctx context.Context, db dbExecutor, userID string, item objectRecord, eventType, source, at string, changes []objectHistoryChange) error {
	id, err := randomID()
	if err != nil {
		return err
	}
	if _, err = db.ExecContext(ctx, `INSERT INTO object_history_events(id,game_id,object_id,object_name,user_id,event_type,source,created_at) VALUES(?,?,?,?,?,?,?,?)`, id, item.GameID, item.ID, item.Name, userID, eventType, source, at); err != nil {
		return err
	}
	for _, change := range changes {
		if _, err = db.ExecContext(ctx, `INSERT INTO object_history_changes(event_id,field_id,field_name,field_type,before_field_type,before_json,after_json) VALUES(?,?,?,?,?,?,?)`, id, change.FieldID, change.FieldName, change.FieldType, change.BeforeFieldType, nullableJSON(change.Before), nullableJSON(change.After)); err != nil {
			return err
		}
	}
	return nil
}

func objectValueHistoryChanges(fields []fieldDefinition, before, after map[string]json.RawMessage) []objectHistoryChange {
	changes := make([]objectHistoryChange, 0)
	for _, field := range fields {
		if field.FieldType == "formula" {
			continue
		}
		oldValue, oldOK := before[field.ID]
		newValue, newOK := after[field.ID]
		if oldOK == newOK && string(oldValue) == string(newValue) {
			continue
		}
		changes = append(changes, objectHistoryChange{
			FieldID: field.ID, FieldName: field.Name, FieldType: field.FieldType,
			Before: cloneRaw(oldValue, oldOK), After: cloneRaw(newValue, newOK),
		})
	}
	return changes
}

func cloneRaw(value json.RawMessage, ok bool) json.RawMessage {
	if !ok || len(value) == 0 {
		return nil
	}
	return append(json.RawMessage(nil), value...)
}

func metadataHistoryChange(name string, before, after string) (objectHistoryChange, bool) {
	if before == after {
		return objectHistoryChange{}, false
	}
	oldJSON, _ := json.Marshal(before)
	newJSON, _ := json.Marshal(after)
	return objectHistoryChange{FieldName: name, FieldType: "object", Before: oldJSON, After: newJSON}, true
}

func (a *API) listObjectHistory(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadObjectAny(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	limit := queryInt(r, "limit", 20, 1, 100)
	offset := queryInt(r, "offset", 0, 0, 1000000)
	query := r.URL.Query()
	eventType := query.Get("event_type")
	if eventType != "" && eventType != "created" && eventType != "updated" && eventType != "archived" && eventType != "restored" && eventType != "field_type_migrated" {
		writeError(w, http.StatusBadRequest, "invalid_history_filter", "Choose a supported history event type.")
		return
	}
	fieldID := query.Get("field_id")
	if len(fieldID) > 200 {
		writeError(w, http.StatusBadRequest, "invalid_history_filter", "Field ID is too long.")
		return
	}
	dateBound := func(key string) (string, error) {
		value := strings.TrimSpace(query.Get(key))
		if value == "" {
			return "", nil
		}
		parsed, parseErr := time.Parse(time.RFC3339, value)
		if parseErr != nil {
			return "", parseErr
		}
		return historyTime(parsed), nil
	}
	since, err := dateBound("since")
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid_history_filter", "Since must be an RFC3339 timestamp.")
		return
	}
	until, err := dateBound("until")
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid_history_filter", "Until must be an RFC3339 timestamp.")
		return
	}
	where := ` WHERE e.game_id=? AND e.object_id=?`
	args := []any{item.GameID, item.ID}
	if eventType != "" {
		where += ` AND e.event_type=?`
		args = append(args, eventType)
	}
	if fieldID != "" {
		where += ` AND EXISTS (SELECT 1 FROM object_history_changes c WHERE c.event_id=e.id AND c.field_id=?)`
		args = append(args, fieldID)
	}
	if since != "" {
		where += ` AND e.created_at>=?`
		args = append(args, since)
	}
	if until != "" {
		where += ` AND e.created_at<=?`
		args = append(args, until)
	}
	var total int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM object_history_events e`+where, args...).Scan(&total); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not count object history.")
		return
	}
	rows, err := a.db.QueryContext(r.Context(), `SELECT e.id,e.object_name,e.event_type,e.source,e.created_at FROM object_history_events e`+where+` ORDER BY e.created_at DESC,e.id DESC LIMIT ? OFFSET ?`, append(args, limit, offset)...)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history.")
		return
	}
	items := make([]objectHistoryItem, 0, limit)
	for rows.Next() {
		var entry objectHistoryItem
		if err := rows.Scan(&entry.ID, &entry.ObjectName, &entry.EventType, &entry.Source, &entry.CreatedAt); err != nil {
			rows.Close()
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history.")
			return
		}
		entry.Changes = []objectHistoryChange{}
		items = append(items, entry)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history.")
		return
	}
	if err := rows.Close(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history.")
		return
	}
	if len(items) > 0 {
		args := make([]any, len(items))
		placeholders := make([]string, len(items))
		byID := make(map[string]int, len(items))
		for i := range items {
			args[i], placeholders[i], byID[items[i].ID] = items[i].ID, "?", i
		}
		changeRows, err := a.db.QueryContext(r.Context(), `SELECT event_id,field_id,field_name,field_type,before_field_type,before_json,after_json FROM object_history_changes WHERE event_id IN (`+strings.Join(placeholders, ",")+") ORDER BY id", args...)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history details.")
			return
		}
		for changeRows.Next() {
			var eventID, fieldID, fieldName, fieldType, beforeFieldType string
			var beforeJSON, afterJSON []byte
			if err := changeRows.Scan(&eventID, &fieldID, &fieldName, &fieldType, &beforeFieldType, &beforeJSON, &afterJSON); err != nil {
				changeRows.Close()
				writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history details.")
				return
			}
			index, ok := byID[eventID]
			if !ok {
				continue
			}
			change := objectHistoryChange{FieldID: fieldID, FieldName: fieldName, FieldType: fieldType, BeforeFieldType: beforeFieldType}
			if beforeJSON != nil {
				change.Before = append(json.RawMessage(nil), beforeJSON...)
			}
			if afterJSON != nil {
				change.After = append(json.RawMessage(nil), afterJSON...)
			}
			items[index].Changes = append(items[index].Changes, change)
		}
		if err := changeRows.Err(); err != nil {
			changeRows.Close()
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history details.")
			return
		}
		changeRows.Close()
	}
	fieldRows, err := a.db.QueryContext(r.Context(), `SELECT DISTINCT c.field_id,c.field_name,c.field_type FROM object_history_changes c JOIN object_history_events e ON e.id=c.event_id WHERE e.game_id=? AND e.object_id=? AND c.field_id<>'' ORDER BY c.field_name COLLATE NOCASE,c.field_id`, item.GameID, item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history fields.")
		return
	}
	fields := make([]objectHistoryField, 0)
	for fieldRows.Next() {
		var field objectHistoryField
		if err := fieldRows.Scan(&field.FieldID, &field.FieldName, &field.FieldType); err != nil {
			fieldRows.Close()
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history fields.")
			return
		}
		fields = append(fields, field)
	}
	if err := fieldRows.Err(); err != nil {
		fieldRows.Close()
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object history fields.")
		return
	}
	fieldRows.Close()
	writeJSON(w, http.StatusOK, map[string]any{"items": items, "fields": fields, "total": total, "limit": limit, "offset": offset, "has_more": offset+len(items) < total})
}
