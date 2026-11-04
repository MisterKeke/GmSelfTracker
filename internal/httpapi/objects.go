package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"math"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"time"

	"gametracker/internal/formula"
)

type objectSummary struct {
	ID           string `json:"id"`
	Name         string `json:"name"`
	ObjectTypeID string `json:"object_type_id"`
	TypeName     string `json:"type_name"`
	Description  string `json:"description"`
	Image        string `json:"image"`
	UpdatedAt    string `json:"updated_at"`
	CreatedAt    string `json:"created_at"`
}

type objectRecord struct {
	objectSummary
	GameID          string                     `json:"game_id"`
	Archived        bool                       `json:"archived"`
	Fields          []fieldDefinition          `json:"fields"`
	Values          map[string]json.RawMessage `json:"values"`
	FormulaErrors   map[string]string          `json:"formula_errors,omitempty"`
	SectionIDs      []string                   `json:"section_ids,omitempty"`
	SectionProgress *sectionProgressSummary    `json:"section_progress,omitempty"`
}

type dbExecutor interface {
	ExecContext(context.Context, string, ...any) (sql.Result, error)
	QueryContext(context.Context, string, ...any) (*sql.Rows, error)
	QueryRowContext(context.Context, string, ...any) *sql.Row
}

func (a *API) listObjects(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	limit := queryInt(r, "limit", 100, 1, 200)
	offset := queryInt(r, "offset", 0, 0, 1000000)
	includeArchived := r.URL.Query().Get("archived") == "true"
	search := strings.TrimSpace(r.URL.Query().Get("search"))
	if len(search) > 2000 {
		writeError(w, http.StatusBadRequest, "invalid_search", "Object search must be 2000 characters or fewer.")
		return
	}
	typeID := r.URL.Query().Get("type_id")
	sectionID := r.URL.Query().Get("section_id")
	excludeSectionID := r.URL.Query().Get("exclude_section_id")
	excludeObjectID := r.URL.Query().Get("exclude_object_id")
	if sectionID != "" {
		if _, err := a.loadSection(r, sectionID, false); err != nil {
			notFoundOrError(w, err, "Section")
			return
		}
		var sameGame int
		if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM sections WHERE id=? AND game_id=? AND archived=0`, sectionID, gameID).Scan(&sameGame); err != nil || sameGame != 1 {
			writeError(w, http.StatusNotFound, "not_found", "Section not found.")
			return
		}
	}
	if excludeSectionID != "" {
		if _, err := a.loadSection(r, excludeSectionID, false); err != nil {
			notFoundOrError(w, err, "Section")
			return
		}
		var sameGame int
		if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM sections WHERE id=? AND game_id=? AND archived=0`, excludeSectionID, gameID).Scan(&sameGame); err != nil || sameGame != 1 {
			writeError(w, http.StatusNotFound, "not_found", "Section not found.")
			return
		}
	}
	sortBy := r.URL.Query().Get("sort")
	if sortBy == "" {
		sortBy = "updated_at"
	}
	direction := strings.ToLower(r.URL.Query().Get("direction"))
	if direction == "" {
		direction = "desc"
	}
	filters := struct {
		Mode       string         `json:"mode"`
		Conditions []objectFilter `json:"conditions"`
	}{Mode: "all"}
	if raw := r.URL.Query().Get("filters"); raw != "" {
		if len(raw) > 64*1024 || json.Unmarshal([]byte(raw), &filters) != nil {
			writeError(w, http.StatusBadRequest, "invalid_filters", "Object filters must be a valid JSON object.")
			return
		}
		if filters.Mode == "" {
			filters.Mode = "all"
		}
	}
	view := objectView{Search: search, TypeID: typeID, Mode: filters.Mode, Sort: sortBy, Direction: direction, Conditions: filters.Conditions}
	if err := a.validateObjectView(r.Context(), gameID, &view); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_query", err.Error())
		return
	}
	query := `SELECT o.id,o.name,o.object_type_id,t.name,o.description,o.image,o.updated_at,o.created_at,o.archived FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.game_id=?`
	if !includeArchived {
		query += ` AND o.archived=0 AND t.archived=0`
	}
	args := []any{gameID}
	if typeID != "" {
		query += ` AND o.object_type_id=?`
		args = append(args, typeID)
	}
	if search != "" {
		query += ` AND (instr(lower(o.name),lower(?))>0 OR instr(lower(o.description),lower(?))>0 OR instr(lower(t.name),lower(?))>0)`
		args = append(args, search, search, search)
	}
	if sectionID != "" {
		query += ` AND EXISTS (SELECT 1 FROM section_placements p WHERE p.section_id=? AND p.object_id=o.id)`
		args = append(args, sectionID)
	}
	if excludeSectionID != "" {
		query += ` AND NOT EXISTS (SELECT 1 FROM section_placements p WHERE p.section_id=? AND p.object_id=o.id)`
		args = append(args, excludeSectionID)
	}
	if excludeObjectID != "" {
		query += ` AND o.id<>?`
		args = append(args, excludeObjectID)
	}
	baseSort := map[string]string{"name": "o.name", "type_name": "t.name", "created_at": "o.created_at", "updated_at": "o.updated_at"}
	_, hasBaseSort := baseSort[sortBy]
	queryInGo := len(view.Conditions) > 0 || !hasBaseSort
	var total int
	if !queryInGo {
		whereStart := strings.Index(query, " WHERE ")
		if whereStart < 0 {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not count objects.")
			return
		}
		countQuery := `SELECT COUNT(*) FROM objects o JOIN object_types t ON t.id=o.object_type_id` + query[whereStart:]
		if err := a.db.QueryRowContext(r.Context(), countQuery, args...).Scan(&total); err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not count objects.")
			return
		}
		query += ` ORDER BY ` + baseSort[sortBy] + ` COLLATE NOCASE ` + strings.ToUpper(direction) + `,o.id ASC LIMIT ? OFFSET ?`
		args = append(args, limit, offset)
	} else {
		query += ` ORDER BY o.id ASC`
	}
	rows, err := a.db.QueryContext(r.Context(), query, args...)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load objects.")
		return
	}
	baseItems := make([]objectRecord, 0)
	for rows.Next() {
		item, scanErr := scanObjectRecord(rows, gameID)
		if scanErr != nil {
			rows.Close()
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load objects.")
			return
		}
		baseItems = append(baseItems, item)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load objects.")
		return
	}
	if err := rows.Close(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load objects.")
		return
	}
	items := make([]objectRecord, 0)
	if !queryInGo {
		items = baseItems
		if err := a.hydrateObjects(r.Context(), items); err != nil {
			writeError(w, 500, "database_error", "Could not load object data.")
			return
		}
	} else {
		candidates := make([]objectQueryCandidate, 0)
		for start := 0; start < len(baseItems); start += 400 {
			end := start + 400
			if end > len(baseItems) {
				end = len(baseItems)
			}
			batch := baseItems[start:end]
			if err := a.hydrateObjects(r.Context(), batch); err != nil {
				writeError(w, http.StatusInternalServerError, "database_error", "Could not load object data.")
				return
			}
			for i := range batch {
				item := batch[i]
				if objectRecordMatches(item, view) {
					key, ok := objectRecordSortValue(item, view.Sort)
					item.Fields, item.Values, item.FormulaErrors, item.SectionIDs = nil, nil, nil, nil
					candidates = append(candidates, objectQueryCandidate{item: item, sortValue: key, sortValid: ok})
				}
				baseItems[start+i] = objectRecord{}
			}
		}
		sortObjectCandidates(candidates, view.Direction)
		total = len(candidates)
		if offset < total {
			end := offset + limit
			if end > total {
				end = total
			}
			for _, candidate := range candidates[offset:end] {
				items = append(items, candidate.item)
			}
		}
		if err := a.hydrateObjects(r.Context(), items); err != nil {
			writeError(w, 500, "database_error", "Could not load object data.")
			return
		}
	}
	if sectionID != "" {
		if err := a.attachSectionProgress(r.Context(), sectionID, items); err != nil {
			writeError(w, http.StatusInternalServerError, "progress_error", "Could not calculate section progress.")
			return
		}
	}
	var gameTotal int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.game_id=? AND o.archived=0 AND t.archived=0`, gameID).Scan(&gameTotal); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not count game objects.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items, "total": total, "game_total": gameTotal, "limit": limit, "offset": offset, "has_more": offset+len(items) < total})
}

func objectRecordMatches(item objectRecord, view objectView) bool {
	if view.TypeID != "" && item.ObjectTypeID != view.TypeID {
		return false
	}
	search := strings.ToLower(strings.TrimSpace(view.Search))
	if search != "" && !strings.Contains(strings.ToLower(item.Name+" "+item.TypeName+" "+item.Description), search) {
		return false
	}
	if len(view.Conditions) == 0 {
		return true
	}
	matched := 0
	for _, condition := range view.Conditions {
		if objectFieldMatches(item, condition) {
			matched++
		}
	}
	if view.Mode == "any" {
		return matched > 0
	}
	return matched == len(view.Conditions)
}

type objectQueryCandidate struct {
	item      objectRecord
	sortValue any
	sortValid bool
}

func scanObjectRecord(rows *sql.Rows, gameID string) (objectRecord, error) {
	var item objectRecord
	var archived int
	if err := rows.Scan(&item.ID, &item.Name, &item.ObjectTypeID, &item.TypeName, &item.Description, &item.Image, &item.UpdatedAt, &item.CreatedAt, &archived); err != nil {
		return objectRecord{}, err
	}
	item.GameID = gameID
	item.Archived = archived != 0
	return item, nil
}

func objectRecordSortValue(item objectRecord, fieldID string) (any, bool) {
	switch fieldID {
	case "name":
		return item.Name, true
	case "type_name":
		return item.TypeName, true
	case "created_at":
		return item.CreatedAt, true
	case "updated_at":
		return item.UpdatedAt, true
	}
	for _, field := range item.Fields {
		if field.ID != fieldID || field.Archived || item.FormulaErrors[fieldID] != "" {
			continue
		}
		raw, ok := item.Values[fieldID]
		if !ok || string(raw) == "null" {
			return nil, false
		}
		if field.FieldType == "progress" {
			var progress struct{ Current, Max float64 }
			if json.Unmarshal(raw, &progress) != nil || progress.Max <= 0 {
				return nil, false
			}
			return progress.Current / progress.Max, true
		}
		if field.FieldType == "integer" || field.FieldType == "decimal" || field.FieldType == "percentage" || field.FieldType == "formula" {
			number, err := numberFromJSON(raw)
			return number, err == nil
		}
		var decoded any
		if json.Unmarshal(raw, &decoded) != nil {
			return nil, false
		}
		return decoded, true
	}
	return nil, false
}

func sortObjectCandidates(items []objectQueryCandidate, direction string) {
	order := 1
	if direction == "desc" {
		order = -1
	}
	sort.SliceStable(items, func(i, j int) bool {
		left, right := items[i], items[j]
		if left.sortValid != right.sortValid {
			return left.sortValid
		}
		if left.sortValid {
			comparison := 0
			switch x := left.sortValue.(type) {
			case float64:
				if y, ok := right.sortValue.(float64); ok && x != y {
					if x < y {
						comparison = -1
					} else {
						comparison = 1
					}
				}
			case bool:
				if y, ok := right.sortValue.(bool); ok && x != y {
					if !x {
						comparison = -1
					} else {
						comparison = 1
					}
				}
			case string:
				if y, ok := right.sortValue.(string); ok {
					comparison = strings.Compare(strings.ToLower(x), strings.ToLower(y))
				}
			}
			if comparison != 0 {
				return comparison*order < 0
			}
		}
		return left.item.ID < right.item.ID
	})
}

func objectFieldMatches(item objectRecord, condition objectFilter) bool {
	var field *fieldDefinition
	for i := range item.Fields {
		if item.Fields[i].ID == condition.FieldID && !item.Fields[i].Archived {
			field = &item.Fields[i]
			break
		}
	}
	if field == nil || item.FormulaErrors[condition.FieldID] != "" {
		return false
	}
	raw, exists := item.Values[condition.FieldID]
	if !exists || len(raw) == 0 || string(raw) == "null" {
		return false
	}
	var target any
	if err := json.Unmarshal(condition.Value, &target); err != nil && condition.Operator != "checked" && condition.Operator != "unchecked" && condition.Operator != "completed" && condition.Operator != "not_completed" {
		return false
	}
	switch field.FieldType {
	case "integer", "decimal", "percentage", "formula":
		value, err := numericValue(raw)
		if err != nil {
			return false
		}
		bound, ok := target.(float64)
		if !ok {
			return false
		}
		switch condition.Operator {
		case "eq":
			return value == bound
		case "ne":
			return value != bound
		case "gt":
			return value > bound
		case "gte":
			return value >= bound
		case "lt":
			return value < bound
		case "lte":
			return value <= bound
		case "between":
			upper, err := numberFromJSON(condition.ValueTo)
			return err == nil && value >= bound && value <= upper
		}
	case "progress":
		var progress struct{ Current, Max float64 }
		if json.Unmarshal(raw, &progress) != nil || progress.Max <= 0 {
			return false
		}
		percent := progress.Current / progress.Max * 100
		bound, ok := target.(float64)
		switch condition.Operator {
		case "completed":
			return percent >= 100
		case "not_completed":
			return percent < 100
		case "above":
			return ok && percent > bound
		case "below":
			return ok && percent < bound
		}
	case "boolean":
		var value bool
		if json.Unmarshal(raw, &value) != nil {
			return false
		}
		return value == (condition.Operator == "checked")
	case "reference", "references":
		var ids []string
		if json.Unmarshal(raw, &ids) != nil {
			var id string
			if json.Unmarshal(raw, &id) != nil {
				return false
			}
			ids = []string{id}
		}
		want, ok := target.(string)
		if !ok {
			return false
		}
		found := false
		for _, id := range ids {
			if id == want {
				found = true
				break
			}
		}
		if condition.Operator == "eq" {
			return found
		}
		return !found
	case "select":
		var value string
		if json.Unmarshal(raw, &value) != nil {
			return false
		}
		if condition.Operator == "one_of" {
			var options []string
			if json.Unmarshal(condition.Value, &options) != nil {
				return false
			}
			for _, option := range options {
				if value == option {
					return true
				}
			}
			return false
		}
		want, ok := target.(string)
		return ok && (value == want) == (condition.Operator == "eq")
	case "text", "long_text", "date":
		var value string
		if json.Unmarshal(raw, &value) != nil {
			return false
		}
		want, ok := target.(string)
		if !ok {
			return false
		}
		if field.FieldType == "date" {
			switch condition.Operator {
			case "before":
				return value < want
			case "after":
				return value > want
			case "on":
				return value == want
			case "between":
				var upper string
				return json.Unmarshal(condition.ValueTo, &upper) == nil && value >= want && value <= upper
			}
		}
		value, want = strings.ToLower(value), strings.ToLower(want)
		switch condition.Operator {
		case "eq":
			return value == want
		case "contains":
			return strings.Contains(value, want)
		case "not_contains":
			return !strings.Contains(value, want)
		case "starts_with":
			return strings.HasPrefix(value, want)
		}
	}
	return false
}

func (a *API) createObject(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	var input struct {
		ObjectTypeID string                     `json:"object_type_id"`
		Name         string                     `json:"name"`
		Description  string                     `json:"description"`
		Image        string                     `json:"image"`
		SectionID    string                     `json:"section_id"`
		Values       map[string]json.RawMessage `json:"values"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if input.Name == "" || len(input.Name) > 160 {
		writeError(w, http.StatusBadRequest, "invalid_name", "Object name must be between 1 and 160 characters.")
		return
	}
	input.Image = canonicalImageURL(r, input.Image)
	if !a.validateImage(r, input.Image) {
		writeError(w, 400, "invalid_image", "Choose an image URL or an upload you own.")
		return
	}
	var typeExists int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM object_types WHERE id=? AND game_id=? AND archived=0`, input.ObjectTypeID, gameID).Scan(&typeExists); err != nil || typeExists == 0 {
		writeError(w, http.StatusBadRequest, "invalid_type", "Choose an object type from this game.")
		return
	}
	fields, err := a.loadFields(r.Context(), input.ObjectTypeID, false)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load field definitions.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create object.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339)
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not create object.")
		return
	}
	defer tx.Rollback()
	_, err = tx.ExecContext(r.Context(), `INSERT INTO objects(id,game_id,object_type_id,name,description,image,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)`, id, gameID, input.ObjectTypeID, input.Name, input.Description, input.Image, now, now)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create object.")
		return
	}
	if err := a.saveValues(r.Context(), tx, gameID, id, input.ObjectTypeID, fields, input.Values); err != nil {
		writeValidationError(w, err)
		return
	}
	if input.SectionID != "" {
		var sectionExists int
		if err := tx.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM sections WHERE id=? AND game_id=? AND archived=0`, input.SectionID, gameID).Scan(&sectionExists); err != nil || sectionExists == 0 {
			writeError(w, http.StatusBadRequest, "invalid_section", "Choose an active section from this game.")
			return
		}
		_, err = tx.ExecContext(r.Context(), `INSERT INTO section_placements(section_id,game_id,object_id,created_at) VALUES(?,?,?,?)`, input.SectionID, gameID, id, now)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "placement_failed", "Could not place object in section.")
			return
		}
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create object.")
		return
	}
	item := objectRecord{objectSummary: objectSummary{ID: id, Name: input.Name, ObjectTypeID: input.ObjectTypeID, Description: input.Description, Image: input.Image, UpdatedAt: now, CreatedAt: now}, GameID: gameID, Fields: fields}
	item.Values, err = a.objectValues(r.Context(), a.db, item, fields)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Object was saved but its values could not be loaded.")
		return
	}
	item.FormulaErrors = formulaErrors(fields, item.Values)
	writeJSON(w, http.StatusCreated, item)
}

func (a *API) getObject(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadObject(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	item.Fields, err = a.loadFields(r.Context(), item.ObjectTypeID, false)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load fields.")
		return
	}
	item.Values, err = a.objectValues(r.Context(), a.db, item, item.Fields)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load values.")
		return
	}
	item.FormulaErrors = formulaErrors(item.Fields, item.Values)
	item.SectionIDs, err = a.objectSections(r.Context(), item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load sections.")
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (a *API) updateObject(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadObject(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	var input struct {
		Name        *string                    `json:"name"`
		Description *string                    `json:"description"`
		Image       *string                    `json:"image"`
		Values      map[string]json.RawMessage `json:"values"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.Name != nil {
		item.Name = strings.TrimSpace(*input.Name)
		if item.Name == "" || len(item.Name) > 160 {
			writeError(w, http.StatusBadRequest, "invalid_name", "Object name must be between 1 and 160 characters.")
			return
		}
	}
	if input.Description != nil {
		item.Description = *input.Description
	}
	if input.Image != nil {
		item.Image = *input.Image
	}
	item.UpdatedAt = time.Now().UTC().Format(time.RFC3339)
	item.Image = canonicalImageURL(r, item.Image)
	if !a.validateImage(r, item.Image) {
		writeError(w, 400, "invalid_image", "Choose an image URL or an upload you own.")
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, 500, "update_failed", "Could not update object.")
		return
	}
	defer tx.Rollback()
	if input.Values != nil {
		fields, err := loadFieldsFrom(r.Context(), tx, item.ObjectTypeID, false)
		if err != nil {
			writeError(w, 500, "database_error", "Could not load fields.")
			return
		}
		item.Fields = fields
		if err := a.saveValues(r.Context(), tx, item.GameID, item.ID, item.ObjectTypeID, fields, input.Values); err != nil {
			writeValidationError(w, err)
			return
		}
	}
	_, err = tx.ExecContext(r.Context(), `UPDATE objects SET name=?,description=?,image=?,updated_at=? WHERE id=?`, item.Name, item.Description, item.Image, item.UpdatedAt, item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update object.")
		return
	}
	if input.Values != nil {
		item.Values, err = a.objectValues(r.Context(), tx, item, item.Fields)
		if err != nil {
			writeError(w, 500, "update_failed", "Could not read updated object values.")
			return
		}
		item.FormulaErrors = formulaErrors(item.Fields, item.Values)
	}
	if err := tx.Commit(); err != nil {
		writeError(w, 500, "update_failed", "Could not update object.")
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (a *API) updateObjectValues(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadObject(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	var input struct {
		Values map[string]json.RawMessage `json:"values"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.Values == nil {
		writeError(w, http.StatusBadRequest, "invalid_values", "Provide a values object keyed by field ID.")
		return
	}
	item.Fields, err = a.loadFields(r.Context(), item.ObjectTypeID, false)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load field definitions.")
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not update values.")
		return
	}
	defer tx.Rollback()
	if err := a.saveValues(r.Context(), tx, item.GameID, item.ID, item.ObjectTypeID, item.Fields, input.Values); err != nil {
		writeValidationError(w, err)
		return
	}
	_, err = tx.ExecContext(r.Context(), `UPDATE objects SET updated_at=? WHERE id=?`, time.Now().UTC().Format(time.RFC3339), item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update values.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update values.")
		return
	}
	item.Values, err = a.objectValues(r.Context(), a.db, item, item.Fields)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Values saved but could not be read back.")
		return
	}
	item.FormulaErrors = formulaErrors(item.Fields, item.Values)
	writeJSON(w, http.StatusOK, map[string]any{"values": item.Values, "formula_errors": item.FormulaErrors})
}

func (a *API) archiveObject(w http.ResponseWriter, r *http.Request) { a.setObjectArchived(w, r, true) }
func (a *API) restoreObject(w http.ResponseWriter, r *http.Request) { a.setObjectArchived(w, r, false) }
func (a *API) setObjectArchived(w http.ResponseWriter, r *http.Request, archived bool) {
	item, err := a.loadObjectAny(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	_, err = a.db.ExecContext(r.Context(), `UPDATE objects SET archived=?,updated_at=? WHERE id=?`, boolInt(archived), time.Now().UTC().Format(time.RFC3339), item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update object.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) deleteObject(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadObject(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "delete_failed", "Could not delete object.")
		return
	}
	defer tx.Rollback()
	// Keep linked goals as manual goals with their existing completion state.
	if _, err = tx.ExecContext(r.Context(), `UPDATE goals SET object_id=NULL,field_id=NULL,target_json=NULL,goal_type='manual',updated_at=? WHERE object_id=?`, time.Now().UTC().Format(time.RFC3339), item.ID); err != nil {
		writeError(w, http.StatusInternalServerError, "delete_failed", "Could not unlink object goals.")
		return
	}
	if _, err = tx.ExecContext(r.Context(), `DELETE FROM objects WHERE id=?`, item.ID); err != nil {
		writeError(w, http.StatusInternalServerError, "delete_failed", "Could not delete object.")
		return
	}
	if err = tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "delete_failed", "Could not delete object.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) loadObject(r *http.Request, id string) (objectRecord, error) {
	var item objectRecord
	err := a.db.QueryRowContext(r.Context(), `SELECT o.id,o.name,o.object_type_id,t.name,o.description,o.image,o.updated_at,o.created_at,o.game_id FROM objects o JOIN object_types t ON t.id=o.object_type_id JOIN games g ON g.id=o.game_id WHERE o.id=? AND g.user_id=? AND o.archived=0 AND t.archived=0 AND g.archived=0`, id, a.currentUser(r).ID).Scan(&item.ID, &item.Name, &item.ObjectTypeID, &item.TypeName, &item.Description, &item.Image, &item.UpdatedAt, &item.CreatedAt, &item.GameID)
	return item, err
}

func (a *API) loadObjectAny(r *http.Request, id string) (objectRecord, error) {
	var item objectRecord
	var archived int
	err := a.db.QueryRowContext(r.Context(), `SELECT o.id,o.name,o.object_type_id,t.name,o.description,o.image,o.updated_at,o.created_at,o.game_id,o.archived FROM objects o JOIN object_types t ON t.id=o.object_type_id JOIN games g ON g.id=o.game_id WHERE o.id=? AND g.user_id=? AND g.archived=0`, id, a.currentUser(r).ID).Scan(&item.ID, &item.Name, &item.ObjectTypeID, &item.TypeName, &item.Description, &item.Image, &item.UpdatedAt, &item.CreatedAt, &item.GameID, &archived)
	item.Archived = archived != 0
	return item, err
}

func (a *API) saveValues(ctx context.Context, db dbExecutor, gameID, objectID, typeID string, fields []fieldDefinition, values map[string]json.RawMessage) error {
	fieldByID := make(map[string]fieldDefinition, len(fields))
	for _, field := range fields {
		fieldByID[field.ID] = field
	}
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, fieldID := range keys {
		field, ok := fieldByID[fieldID]
		if !ok {
			return validationError{code: "unknown_field", message: "A value refers to an unknown or archived field."}
		}
		if field.FieldType == "formula" {
			return validationError{code: "formula_read_only", message: "Calculated fields are read-only."}
		}
		validated, refs, empty, err := validateFieldValue(field, values[fieldID])
		if err != nil {
			return err
		}
		if field.FieldType == "reference" || field.FieldType == "references" {
			for _, ref := range refs {
				var targetType string
				var archived int
				if err := db.QueryRowContext(ctx, `SELECT o.object_type_id,CASE WHEN o.archived=1 OR t.archived=1 THEN 1 ELSE 0 END FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.id=? AND o.game_id=?`, ref, gameID).Scan(&targetType, &archived); err != nil {
					return validationError{code: "invalid_reference", message: "References must point to objects in the same game."}
				}
				if archived != 0 {
					var existing int
					if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM object_relationships WHERE object_id=? AND field_id=? AND referenced_object_id=? AND game_id=?`, objectID, field.ID, ref, gameID).Scan(&existing); err != nil || existing == 0 {
						return validationError{code: "invalid_reference", message: "New references must point to active objects in the same game."}
					}
				}
				var options map[string]any
				_ = json.Unmarshal(field.Options, &options)
				if allowed, ok := options["allowed_type_id"].(string); ok && allowed != "" && targetType != allowed {
					return validationError{code: "invalid_reference_type", message: "This field only accepts objects of the configured type."}
				}
			}
			if _, err := db.ExecContext(ctx, `DELETE FROM object_relationships WHERE object_id=? AND field_id=?`, objectID, field.ID); err != nil {
				return err
			}
			for _, ref := range refs {
				if _, err := db.ExecContext(ctx, `INSERT INTO object_relationships(game_id,object_id,object_type_id,field_id,referenced_object_id) VALUES(?,?,?,?,?)`, gameID, objectID, typeID, field.ID, ref); err != nil {
					return err
				}
			}
			if _, err := db.ExecContext(ctx, `DELETE FROM field_values WHERE object_id=? AND field_id=?`, objectID, field.ID); err != nil {
				return err
			}
			continue
		}
		if empty {
			if _, err := db.ExecContext(ctx, `DELETE FROM field_values WHERE object_id=? AND field_id=?`, objectID, field.ID); err != nil {
				return err
			}
			continue
		}
		if _, err := db.ExecContext(ctx, `INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?) ON CONFLICT(object_id,field_id) DO UPDATE SET value_json=excluded.value_json,updated_at=excluded.updated_at`, objectID, typeID, field.ID, string(validated), time.Now().UTC().Format(time.RFC3339)); err != nil {
			return err
		}
	}
	return nil
}

func validateFieldValue(field fieldDefinition, raw json.RawMessage) (json.RawMessage, []string, bool, error) {
	if len(raw) == 0 || string(raw) == "null" {
		if field.FieldType == "reference" || field.FieldType == "references" {
			return nil, nil, true, nil
		}
		return nil, nil, true, nil
	}
	var value any
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.UseNumber()
	if err := decoder.Decode(&value); err != nil {
		return nil, nil, false, validationError{code: "invalid_value", message: "Field value must be valid JSON."}
	}
	invalid := func(message string) (json.RawMessage, []string, bool, error) {
		return nil, nil, false, validationError{code: "invalid_value", message: message}
	}
	switch field.FieldType {
	case "text", "long_text":
		text, ok := value.(string)
		if !ok {
			return invalid("Enter text for this field.")
		}
		max := 2000
		if field.FieldType == "long_text" {
			max = 50000
		}
		if len(text) > max {
			return invalid(fmt.Sprintf("Text is too long (maximum %d characters).", max))
		}
	case "integer":
		number, ok := value.(json.Number)
		if !ok {
			return invalid("Enter a whole number.")
		}
		if _, err := strconv.ParseInt(string(number), 10, 64); err != nil {
			return invalid("Enter a whole number within the supported range.")
		}
	case "decimal", "percentage":
		n, ok := value.(json.Number)
		if !ok {
			return invalid("Enter a number.")
		}
		f, err := strconv.ParseFloat(string(n), 64)
		if err != nil || math.IsNaN(f) || math.IsInf(f, 0) {
			return invalid("Enter a valid finite number.")
		}
		if field.FieldType == "percentage" && (f < 0 || f > 100) {
			return invalid("Percentage must be between 0 and 100.")
		}
	case "boolean":
		if _, ok := value.(bool); !ok {
			return invalid("Choose true or false.")
		}
	case "select":
		choice, ok := value.(string)
		if !ok {
			return invalid("Choose one of the available options.")
		}
		var options struct {
			Choices []string `json:"choices"`
		}
		_ = json.Unmarshal(field.Options, &options)
		found := false
		for _, candidate := range options.Choices {
			if choice == candidate {
				found = true
				break
			}
		}
		if !found {
			return invalid("Choose one of the available options.")
		}
	case "date":
		date, ok := value.(string)
		if !ok {
			return invalid("Enter a date.")
		}
		if _, err := time.Parse("2006-01-02", date); err != nil {
			return invalid("Date must use YYYY-MM-DD format.")
		}
	case "progress":
		progress, ok := value.(map[string]any)
		if !ok {
			return invalid("Progress needs current and maximum values.")
		}
		current, cok := progress["current"].(json.Number)
		maximum, mok := progress["max"].(json.Number)
		if !cok || !mok {
			return invalid("Progress needs numeric current and maximum values.")
		}
		maxf, e1 := strconv.ParseFloat(string(maximum), 64)
		_, e2 := strconv.ParseFloat(string(current), 64)
		if e1 != nil || e2 != nil || maxf <= 0 {
			return invalid("Progress maximum must be greater than zero.")
		}
	case "reference":
		id, ok := value.(string)
		if !ok {
			return invalid("Choose one object reference.")
		}
		return json.RawMessage("null"), []string{id}, false, nil
	case "references":
		array, ok := value.([]any)
		if !ok {
			return invalid("Choose a list of object references.")
		}
		refs := make([]string, 0, len(array))
		seen := map[string]bool{}
		for _, part := range array {
			id, ok := part.(string)
			if !ok {
				return invalid("Every reference must be an object ID.")
			}
			if !seen[id] {
				refs = append(refs, id)
				seen[id] = true
			}
		}
		return json.RawMessage("null"), refs, false, nil
	default:
		return invalid("This field type does not accept values.")
	}
	encoded, err := json.Marshal(value)
	if err != nil {
		return invalid("Could not store this value.")
	}
	return encoded, nil, false, nil
}

func (a *API) objectValues(ctx context.Context, db dbExecutor, item objectRecord, fields []fieldDefinition) (map[string]json.RawMessage, error) {
	values := make(map[string]json.RawMessage, len(fields))
	rows, err := db.QueryContext(ctx, `SELECT field_id,value_json FROM field_values WHERE object_id=?`, item.ID)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var fieldID string
		var raw []byte
		if err := rows.Scan(&fieldID, &raw); err != nil {
			rows.Close()
			return nil, err
		}
		values[fieldID] = append(json.RawMessage(nil), raw...)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	rows, err = db.QueryContext(ctx, `SELECT field_id,referenced_object_id FROM object_relationships WHERE object_id=? ORDER BY field_id,referenced_object_id`, item.ID)
	if err != nil {
		return nil, err
	}
	refs := map[string][]string{}
	for rows.Next() {
		var fieldID, refID string
		if err := rows.Scan(&fieldID, &refID); err != nil {
			rows.Close()
			return nil, err
		}
		refs[fieldID] = append(refs[fieldID], refID)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	if err := a.finishObjectValues(ctx, db, item, fields, values, refs); err != nil {
		return nil, err
	}
	return values, nil
}

func (a *API) finishObjectValues(ctx context.Context, db dbExecutor, item objectRecord, fields []fieldDefinition, values map[string]json.RawMessage, refs map[string][]string) error {
	for _, field := range fields {
		if refs, ok := refs[field.ID]; ok {
			if field.FieldType == "reference" && len(refs) > 0 {
				raw, _ := json.Marshal(refs[0])
				values[field.ID] = raw
			} else {
				raw, _ := json.Marshal(refs)
				values[field.ID] = raw
			}
		} else if _, ok := values[field.ID]; !ok && len(field.DefaultValue) > 0 {
			values[field.ID] = append(json.RawMessage(nil), field.DefaultValue...)
		}
	}
	if err := a.calculateFormulas(ctx, db, item, fields, values); err != nil {
		return err
	}
	return nil
}

func (a *API) calculateFormulas(ctx context.Context, db dbExecutor, item objectRecord, fields []fieldDefinition, values map[string]json.RawMessage) error {
	fieldMap := map[string]fieldDefinition{}
	for _, field := range fields {
		fieldMap[field.ID] = field
	}
	numeric := map[string]float64{}
	for id, raw := range values {
		var n json.Number
		decoder := json.NewDecoder(strings.NewReader(string(raw)))
		decoder.UseNumber()
		if decoder.Decode(&n) == nil {
			if value, err := strconv.ParseFloat(string(n), 64); err == nil {
				numeric[id] = value
			} else {
				_ = n
			}
		} else {
			var progress struct {
				Current json.Number `json:"current"`
			}
			if json.Unmarshal(raw, &progress) == nil {
				if value, err := strconv.ParseFloat(string(progress.Current), 64); err == nil {
					numeric[id] = value
				}
			}
		}
	}
	visiting := map[string]bool{}
	done := map[string]bool{}
	var evaluate func(string) error
	evaluate = func(id string) error {
		field, ok := fieldMap[id]
		if !ok || field.FieldType != "formula" || done[id] {
			return nil
		}
		if len(visiting) > 128 {
			return fmt.Errorf("formula dependency limit exceeded")
		}
		if visiting[id] {
			return fmt.Errorf("circular formula dependency")
		}
		visiting[id] = true
		var cfg struct {
			Expression  string                   `json:"expression"`
			Aggregation *relationshipAggregation `json:"aggregation"`
		}
		if err := json.Unmarshal(field.Options, &cfg); err != nil {
			return err
		}
		for _, ref := range formula.References(cfg.Expression) {
			if dep, ok := fieldMap[ref]; ok && dep.FieldType == "formula" {
				if err := evaluate(ref); err != nil {
					return err
				}
			}
		}
		inputs := make(map[string]float64, len(numeric)+1)
		for key, value := range numeric {
			inputs[key] = value
		}
		if cfg.Aggregation != nil {
			aggregated, err := a.relationshipAggregate(ctx, db, item, *cfg.Aggregation)
			if err != nil {
				delete(visiting, id)
				return nil
			}
			inputs["related"] = aggregated
		}
		result, err := formula.Evaluate(cfg.Expression, inputs)
		if err != nil {
			delete(visiting, id)
			return nil
		}
		encoded, _ := json.Marshal(result)
		values[id] = encoded
		numeric[id] = result
		delete(visiting, id)
		done[id] = true
		return nil
	}
	for _, field := range fields {
		if field.FieldType == "formula" {
			if err := evaluate(field.ID); err != nil {
				return err
			}
		}
	}
	return nil
}

func formulaErrors(fields []fieldDefinition, values map[string]json.RawMessage) map[string]string {
	fieldMap := make(map[string]fieldDefinition, len(fields))
	for _, field := range fields {
		fieldMap[field.ID] = field
	}
	visiting := map[string]bool{}
	memo := map[string]float64{}
	done := map[string]bool{}
	var resolve func(string) (float64, error)
	resolve = func(id string) (float64, error) {
		if done[id] {
			return memo[id], nil
		}
		field, ok := fieldMap[id]
		if !ok {
			return 0, fmt.Errorf("field %s is unavailable", id)
		}
		if visiting[id] {
			return 0, fmt.Errorf("circular formula dependency")
		}
		visiting[id] = true
		defer delete(visiting, id)
		if field.FieldType == "formula" {
			if raw, exists := values[id]; exists {
				if number, err := numericValue(raw); err == nil {
					memo[id] = number
					done[id] = true
					return number, nil
				}
			}
			if len(visiting) > 128 {
				return 0, fmt.Errorf("formula dependency limit exceeded")
			}
			var config struct {
				Expression string `json:"expression"`
			}
			if err := json.Unmarshal(field.Options, &config); err != nil {
				return 0, fmt.Errorf("formula configuration is invalid")
			}
			numeric := map[string]float64{}
			for _, ref := range formula.References(config.Expression) {
				number, err := resolve(ref)
				if err != nil {
					return 0, err
				}
				numeric[ref] = number
			}
			number, err := formula.Evaluate(config.Expression, numeric)
			if err != nil {
				return 0, err
			}
			memo[id] = number
			done[id] = true
			return number, nil
		}
		raw, exists := values[id]
		if !exists || string(raw) == "null" {
			return 0, fmt.Errorf("enter a value for %s", field.Name)
		}
		var number float64
		var err error
		if field.FieldType == "progress" {
			var progress struct {
				Current json.Number `json:"current"`
			}
			if json.Unmarshal(raw, &progress) != nil {
				return 0, fmt.Errorf("%s needs a current number", field.Name)
			}
			number, err = strconv.ParseFloat(string(progress.Current), 64)
		} else {
			number, err = numberFromJSON(raw)
		}
		if err != nil || math.IsNaN(number) || math.IsInf(number, 0) {
			return 0, fmt.Errorf("%s has no numeric value", field.Name)
		}
		memo[id] = number
		done[id] = true
		return number, nil
	}
	errorsByField := map[string]string{}
	for _, field := range fields {
		if field.FieldType != "formula" {
			continue
		}
		if _, err := resolve(field.ID); err != nil {
			errorsByField[field.ID] = err.Error()
		}
	}
	return errorsByField
}

func (a *API) objectSections(ctx context.Context, id string) ([]string, error) {
	rows, err := a.db.QueryContext(ctx, `SELECT section_id FROM section_placements WHERE object_id=? ORDER BY section_id`, id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]string, 0)
	for rows.Next() {
		var sectionID string
		if err := rows.Scan(&sectionID); err != nil {
			return nil, err
		}
		items = append(items, sectionID)
	}
	return items, rows.Err()
}

type validationError struct{ code, message string }

func (e validationError) Error() string { return e.message }
func writeValidationError(w http.ResponseWriter, err error) {
	if v, ok := err.(validationError); ok {
		writeError(w, http.StatusBadRequest, v.code, v.message)
		return
	}
	writeError(w, http.StatusInternalServerError, "database_error", "Could not save the requested changes.")
}
