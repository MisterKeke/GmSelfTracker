package httpapi

import (
	"context"
	"encoding/json"
	"strings"
)

type readCacheKey struct{}
type fieldCacheKey struct {
	id       string
	archived bool
}
type cachedObject struct {
	record objectRecord
	err    error
}
type readCache struct {
	fields  map[fieldCacheKey][]fieldDefinition
	objects map[string]cachedObject
}

func withReadCache(ctx context.Context) context.Context {
	return context.WithValue(ctx, readCacheKey{}, &readCache{fields: map[fieldCacheKey][]fieldDefinition{}, objects: map[string]cachedObject{}})
}

// Cache only within a read request. Mutations must always read the current schema.
func (a *API) goalObject(ctx context.Context, gameID, objectID string) (objectRecord, error) {
	cache, _ := ctx.Value(readCacheKey{}).(*readCache)
	key := gameID + ":" + objectID
	if cache != nil {
		if value, ok := cache.objects[key]; ok {
			return value.record, value.err
		}
	}
	var record objectRecord
	err := a.db.QueryRowContext(ctx, `SELECT o.id,o.game_id,o.object_type_id FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.id=? AND o.game_id=? AND o.archived=0 AND t.archived=0`, objectID, gameID).Scan(&record.ID, &record.GameID, &record.ObjectTypeID)
	if err == nil {
		record.Fields, err = a.loadFields(ctx, record.ObjectTypeID, false)
	}
	if err == nil {
		record.Values, err = a.objectValues(ctx, a.db, record, record.Fields)
	}
	if cache != nil {
		cache.objects[key] = cachedObject{record: record, err: err}
	}
	return record, err
}

// A page uses three bulk queries for values, relationships and placements, and
// loads each type's field definitions once instead of once per object.
func (a *API) hydrateObjects(ctx context.Context, items []objectRecord) error {
	if len(items) == 0 {
		return nil
	}
	indexes := map[string]int{}
	args := make([]any, len(items))
	refs := make([]map[string][]string, len(items))
	fields := map[string][]fieldDefinition{}
	for i := range items {
		indexes[items[i].ID], args[i] = i, items[i].ID
		items[i].Values = map[string]json.RawMessage{}
		refs[i] = map[string][]string{}
		if _, ok := fields[items[i].ObjectTypeID]; !ok {
			var err error
			fields[items[i].ObjectTypeID], err = a.loadFields(ctx, items[i].ObjectTypeID, false)
			if err != nil {
				return err
			}
		}
		items[i].Fields = fields[items[i].ObjectTypeID]
	}
	placeholders := strings.TrimSuffix(strings.Repeat("?,", len(items)), ",")
	queries := []string{
		`SELECT object_id,field_id,value_json FROM field_values WHERE object_id IN (` + placeholders + `)`,
		`SELECT object_id,field_id,referenced_object_id FROM object_relationships WHERE object_id IN (` + placeholders + `) ORDER BY field_id,referenced_object_id`,
		`SELECT object_id,section_id FROM section_placements WHERE object_id IN (` + placeholders + `) ORDER BY section_id`,
	}
	for kind, query := range queries {
		rows, err := a.db.QueryContext(ctx, query, args...)
		if err != nil {
			return err
		}
		for rows.Next() {
			var objectID, fieldID, target string
			var raw []byte
			switch kind {
			case 0:
				err = rows.Scan(&objectID, &fieldID, &raw)
				if err == nil {
					items[indexes[objectID]].Values[fieldID] = append(json.RawMessage(nil), raw...)
				}
			case 1:
				err = rows.Scan(&objectID, &fieldID, &target)
				if err == nil {
					refs[indexes[objectID]][fieldID] = append(refs[indexes[objectID]][fieldID], target)
				}
			case 2:
				err = rows.Scan(&objectID, &target)
				if err == nil {
					items[indexes[objectID]].SectionIDs = append(items[indexes[objectID]].SectionIDs, target)
				}
			}
			if err != nil {
				rows.Close()
				return err
			}
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		if err := rows.Close(); err != nil {
			return err
		}
	}
	for i := range items {
		if err := a.finishObjectValues(ctx, a.db, items[i], items[i].Fields, items[i].Values, refs[i]); err != nil {
			return err
		}
		items[i].FormulaErrors = formulaErrors(items[i].Fields, items[i].Values)
	}
	return nil
}
