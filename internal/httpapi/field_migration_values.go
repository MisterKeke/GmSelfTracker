package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"math/big"
	"strconv"
	"strings"
	"time"
)

func (a *API) inspectStoredFieldValues(ctx context.Context, db dbExecutor, gameID string, current, target fieldDefinition, policy map[string]json.RawMessage, summary *fieldMigrationSummary) error {
	rows, err := db.QueryContext(ctx, `SELECT o.id,o.name,v.value_json FROM field_values v JOIN objects o ON o.id=v.object_id WHERE v.field_id=? ORDER BY o.id`, current.ID)
	if err != nil {
		return err
	}
	for rows.Next() {
		var objectID, objectName string
		var raw []byte
		if err := rows.Scan(&objectID, &objectName, &raw); err != nil {
			rows.Close()
			return err
		}
		summary.SavedValues++
		converted, err := convertFieldTypeValue(current.FieldType, target, policy, raw)
		if err == nil && (target.FieldType == "reference" || target.FieldType == "references") {
			err = validateMigrationReferenceValue(ctx, db, gameID, target, converted)
		}
		if err != nil {
			summary.IncompatibleValues++
			if len(summary.Examples) < 10 {
				summary.Examples = append(summary.Examples, fieldMigrationExample{ObjectID: objectID, ObjectName: objectName, Value: append(json.RawMessage(nil), raw...), Reason: err.Error()})
			}
		}
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	return rows.Close()
}

func validateMigrationReferenceValue(ctx context.Context, db dbExecutor, gameID string, target fieldDefinition, raw json.RawMessage) error {
	ids := []string{}
	if target.FieldType == "reference" {
		var id string
		if err := json.Unmarshal(raw, &id); err != nil || id == "" {
			return fmt.Errorf("Choose a target object for each converted text value.")
		}
		ids = append(ids, id)
	} else {
		if err := json.Unmarshal(raw, &ids); err != nil || len(ids) == 0 {
			return fmt.Errorf("Choose a target object for each converted text value.")
		}
	}
	allowedTypeID := ""
	var options struct {
		AllowedTypeID string `json:"allowed_type_id"`
	}
	_ = json.Unmarshal(target.Options, &options)
	allowedTypeID = options.AllowedTypeID
	for _, id := range ids {
		var count int
		if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.id=? AND o.game_id=? AND o.archived=0 AND t.archived=0 AND (?='' OR o.object_type_id=?)`, id, gameID, allowedTypeID, allowedTypeID).Scan(&count); err != nil {
			return err
		}
		if count != 1 {
			return fmt.Errorf("Mapped object must be active, belong to this game, and match the allowed target type.")
		}
	}
	return nil
}

func (a *API) inspectFieldRelationships(ctx context.Context, db dbExecutor, current, target fieldDefinition, policy map[string]json.RawMessage, summary *fieldMigrationSummary) error {
	allowedTypeID := ""
	if target.FieldType == "reference" || target.FieldType == "references" {
		var options struct {
			AllowedTypeID string `json:"allowed_type_id"`
		}
		_ = json.Unmarshal(target.Options, &options)
		allowedTypeID = options.AllowedTypeID
	}
	mismatched := map[string]string{}
	if allowedTypeID != "" {
		mismatchRows, err := db.QueryContext(ctx, `SELECT DISTINCT o.id,o.name FROM objects o JOIN object_relationships rel ON rel.object_id=o.id JOIN objects target ON target.id=rel.referenced_object_id WHERE rel.field_id=? AND target.object_type_id<>? ORDER BY o.id`, current.ID, allowedTypeID)
		if err != nil {
			return err
		}
		for mismatchRows.Next() {
			var id, name string
			if err := mismatchRows.Scan(&id, &name); err != nil {
				mismatchRows.Close()
				return err
			}
			mismatched[id] = name
		}
		if err := mismatchRows.Err(); err != nil {
			mismatchRows.Close()
			return err
		}
		if err := mismatchRows.Close(); err != nil {
			return err
		}
	}
	rows, err := db.QueryContext(ctx, `SELECT o.id,o.name,json_group_array(rel.referenced_object_id) FROM objects o JOIN object_relationships rel ON rel.object_id=o.id WHERE rel.field_id=? GROUP BY o.id,o.name ORDER BY o.id`, current.ID)
	if err != nil {
		return err
	}
	for rows.Next() {
		var objectID, objectName string
		var rawRefs []byte
		if err := rows.Scan(&objectID, &objectName, &rawRefs); err != nil {
			rows.Close()
			return err
		}
		summary.SavedValues++
		var refs []string
		if err := json.Unmarshal(rawRefs, &refs); err != nil {
			rows.Close()
			return err
		}
		_, err := convertReferenceMigrationValue(current.FieldType, target, policy, refs)
		if allowedTypeID != "" && mismatched[objectID] != "" {
			err = fmt.Errorf("One or more linked objects do not match the target allowed type.")
		}
		if err != nil {
			summary.IncompatibleValues++
			if len(summary.Examples) < 10 {
				encoded, _ := json.Marshal(refs)
				summary.Examples = append(summary.Examples, fieldMigrationExample{ObjectID: objectID, ObjectName: objectName, Value: encoded, Reason: err.Error()})
			}
		}
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	return rows.Close()
}

func convertReferenceMigrationValue(from string, target fieldDefinition, policy map[string]json.RawMessage, refs []string) (json.RawMessage, error) {
	if target.FieldType == "reference" || target.FieldType == "references" {
		return convertReferenceFieldValue(from, target.FieldType, refs)
	}
	if (target.FieldType == "text" || target.FieldType == "long_text") && policyString(policy, "relationship_strategy") == "serialize_ids" {
		encoded, err := json.Marshal(refs)
		if err != nil {
			return nil, err
		}
		raw, err := json.Marshal(string(encoded))
		if err != nil {
			return nil, err
		}
		validated, _, _, err := validateFieldValue(target, raw)
		return validated, err
	}
	return nil, fmt.Errorf("Choose the explicit serialize linked IDs strategy to preserve these relationships as text.")
}

func (a *API) inspectFormulaFieldValues(ctx context.Context, db dbExecutor, gameID string, current, target fieldDefinition, policy map[string]json.RawMessage, summary *fieldMigrationSummary) error {
	var staleRows int
	if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM field_values WHERE field_id=?`, current.ID).Scan(&staleRows); err != nil {
		return err
	}
	if staleRows > 0 {
		summary.Blockers = append(summary.Blockers, "This formula field has unexpected stored rows. Resolve them before freezing the calculated result.")
		var objectID, objectName string
		var value []byte
		if err := db.QueryRowContext(ctx, `SELECT o.id,o.name,v.value_json FROM field_values v JOIN objects o ON o.id=v.object_id WHERE v.field_id=? ORDER BY o.id LIMIT 1`, current.ID).Scan(&objectID, &objectName, &value); err == nil {
			summary.Examples = append(summary.Examples, fieldMigrationExample{ObjectID: objectID, ObjectName: objectName, Value: value, Reason: "Unexpected stored data cannot be overwritten by a formula freeze."})
		}
	}
	const batchSize = 200
	lastID := ""
	for {
		rows, err := db.QueryContext(ctx, `SELECT o.id,o.name,o.object_type_id,t.name,o.description,o.image,o.updated_at,o.created_at,o.archived FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.object_type_id=? AND o.id>? ORDER BY o.id LIMIT ?`, current.ObjectTypeID, lastID, batchSize)
		if err != nil {
			return err
		}
		batch := make([]objectRecord, 0, batchSize)
		for rows.Next() {
			item, err := scanObjectRecord(rows, gameID)
			if err != nil {
				rows.Close()
				return err
			}
			batch = append(batch, item)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		if err := rows.Close(); err != nil {
			return err
		}
		if len(batch) == 0 {
			return nil
		}
		if err := a.hydrateObjectsBatchFrom(ctx, db, batch); err != nil {
			return err
		}
		for _, item := range batch {
			lastID = item.ID
			summary.SavedValues++
			raw, exists := item.Values[current.ID]
			reason := ""
			if formulaError := item.FormulaErrors[current.ID]; formulaError != "" {
				reason = "Formula result is unavailable: " + formulaError
			} else if !exists || strings.TrimSpace(string(raw)) == "null" {
				reason = "Formula result is missing."
			} else if _, err := convertFieldTypeValue("decimal", target, policy, raw); err != nil {
				reason = err.Error()
			}
			if reason != "" {
				summary.IncompatibleValues++
				if len(summary.Examples) < 10 {
					summary.Examples = append(summary.Examples, fieldMigrationExample{ObjectID: item.ID, ObjectName: item.Name, Value: append(json.RawMessage(nil), raw...), Reason: reason})
				}
			}
		}
	}
}

func convertReferenceFieldValue(from, to string, refs []string) (json.RawMessage, error) {
	if from != "reference" && from != "references" {
		return nil, fmt.Errorf("Stored values need an explicit mapping to object references.")
	}
	if to != "reference" && to != "references" {
		return nil, fmt.Errorf("Object relationships need an explicit mapping before converting this field to %s.", to)
	}
	if from == "reference" && len(refs) > 1 {
		return nil, fmt.Errorf("A single-reference field contains more than one link; resolve it before migrating.")
	}
	if from == "references" && to == "reference" && len(refs) > 1 {
		return nil, fmt.Errorf("This object has more than one reference; select one explicitly before converting to a single reference.")
	}
	if to == "reference" {
		if len(refs) == 0 {
			return json.RawMessage("null"), nil
		}
		return json.Marshal(refs[0])
	}
	return json.Marshal(refs)
}

func convertFieldTypeValue(from string, target fieldDefinition, policy map[string]json.RawMessage, raw []byte) (json.RawMessage, error) {
	if len(raw) == 0 || strings.TrimSpace(string(raw)) == "null" {
		return json.RawMessage("null"), nil
	}
	var value any
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.UseNumber()
	if err := decoder.Decode(&value); err != nil {
		return nil, fmt.Errorf("Saved value is not valid JSON.")
	}
	if from == "formula" || target.FieldType == "formula" || from == "reference" || from == "references" {
		return nil, fmt.Errorf("This conversion needs an explicit formula or relationship migration strategy.")
	}
	if target.FieldType == "reference" || target.FieldType == "references" {
		if from != "text" && from != "long_text" && from != "select" {
			return nil, fmt.Errorf("Only text, long text, or select values can be mapped to an object reference.")
		}
		text, ok := value.(string)
		if !ok {
			return nil, fmt.Errorf("Saved value is not text.")
		}
		id := policyStringMap(policy, "reference_mapping")[text]
		if id == "" {
			return nil, fmt.Errorf("Choose a target object for this text value.")
		}
		if target.FieldType == "reference" {
			return json.Marshal(id)
		}
		return json.Marshal([]string{id})
	}
	textValue := func() (string, error) {
		text, ok := value.(string)
		if !ok {
			return "", fmt.Errorf("Saved value is not text.")
		}
		return text, nil
	}
	numberValue := func() (json.Number, error) {
		if numericType(from) {
			number, ok := value.(json.Number)
			if !ok {
				return "", fmt.Errorf("Saved value is not numeric.")
			}
			return number, nil
		}
		if from == "text" || from == "long_text" {
			text, ok := value.(string)
			if !ok {
				return "", fmt.Errorf("Saved value is not text.")
			}
			text = strings.TrimSpace(text)
			if len(text) > 128 || !json.Valid([]byte(text)) || strings.ContainsAny(text, " \t\r\n") {
				return "", fmt.Errorf("Text is not a valid numeric representation.")
			}
			var parsed any
			dec := json.NewDecoder(strings.NewReader(text))
			dec.UseNumber()
			if dec.Decode(&parsed) != nil {
				return "", fmt.Errorf("Text is not a valid numeric representation.")
			}
			number, ok := parsed.(json.Number)
			if !ok {
				return "", fmt.Errorf("Text is not a valid numeric representation.")
			}
			if _, err := strconv.ParseFloat(string(number), 64); err != nil {
				return "", fmt.Errorf("Number is outside the supported range.")
			}
			return number, nil
		}
		return "", fmt.Errorf("Values from %s cannot be converted to a number automatically.", from)
	}
	var converted any
	switch target.FieldType {
	case "text", "long_text":
		switch {
		case numericType(from):
			number, ok := value.(json.Number)
			if !ok {
				return nil, fmt.Errorf("Saved value is not numeric.")
			}
			canonical, ok := canonicalDecimalText(string(number))
			if !ok {
				return nil, fmt.Errorf("Saved number cannot be represented as text exactly.")
			}
			converted = canonical
		case from == "boolean":
			boolean, ok := value.(bool)
			if !ok {
				return nil, fmt.Errorf("Saved value is not boolean.")
			}
			converted = strconv.FormatBool(boolean)
		case from == "text" || from == "long_text" || from == "select" || from == "date":
			text, err := textValue()
			if err != nil {
				return nil, err
			}
			converted = text
		default:
			return nil, fmt.Errorf("Values from %s cannot be converted to text automatically.", from)
		}
	case "integer", "decimal", "percentage":
		number, err := numberValue()
		if err != nil {
			if from == "progress" {
				number, err = progressNumber(value, target.FieldType, policy)
			}
			if err != nil {
				return nil, err
			}
		}
		if target.FieldType == "percentage" {
			parsed, parseErr := strconv.ParseFloat(string(number), 64)
			if parseErr != nil {
				return nil, fmt.Errorf("Number is outside the supported range.")
			}
			if parsed < 0 || parsed > 100 {
				if policyString(policy, "percentage_strategy") != "clamp" {
					return nil, fmt.Errorf("Value is outside 0–100; choose the explicit clamp strategy or resolve it manually.")
				}
				if parsed < 0 {
					number = json.Number("0")
				} else {
					number = json.Number("100")
				}
			}
		}
		if target.FieldType == "integer" {
			rat, ok := decimalRat(string(number))
			if !ok || rat.Denom().Cmp(big.NewInt(1)) != 0 {
				return nil, fmt.Errorf("Value is not mathematically integral.")
			}
			if !rat.Num().IsInt64() {
				return nil, fmt.Errorf("Integer is outside the supported range.")
			}
			converted = json.Number(rat.Num().String())
		} else {
			converted = number
		}
	case "boolean":
		if from != "text" && from != "long_text" {
			return nil, fmt.Errorf("Only explicit text values true or false can become a checkbox.")
		}
		text, err := textValue()
		if err != nil {
			return nil, err
		}
		switch strings.ToLower(strings.TrimSpace(text)) {
		case "true":
			converted = true
		case "false":
			converted = false
		default:
			return nil, fmt.Errorf("Text must be exactly true or false to become a checkbox.")
		}
	case "select":
		if from != "text" && from != "long_text" {
			return nil, fmt.Errorf("Only text values matching a target choice can become a select value.")
		}
		text, err := textValue()
		if err != nil {
			return nil, err
		}
		var options struct {
			Choices []string `json:"choices"`
		}
		_ = json.Unmarshal(target.Options, &options)
		found := false
		replacements := policyStringMap(policy, "select_replacements")
		if replacement, ok := replacements[text]; ok {
			text = replacement
		}
		for _, choice := range options.Choices {
			if choice == text {
				found = true
				break
			}
		}
		if !found {
			return nil, fmt.Errorf("Value does not match a target select option.")
		}
		converted = text
	case "date":
		if from != "text" && from != "long_text" {
			return nil, fmt.Errorf("Only canonical ISO dates can become date values.")
		}
		text, err := textValue()
		if err != nil {
			return nil, err
		}
		parsed, err := time.Parse("2006-01-02", text)
		if err != nil || parsed.Format("2006-01-02") != text {
			return nil, fmt.Errorf("Value must be a canonical ISO date in YYYY-MM-DD format.")
		}
		converted = text
	case "progress":
		if !numericType(from) {
			return nil, fmt.Errorf("A valid numeric value and maximum are required for a progress field.")
		}
		number, err := numberValue()
		if err != nil {
			return nil, err
		}
		maximum, err := policyNumber(policy, "maximum")
		if err != nil || maximum <= 0 {
			return nil, fmt.Errorf("Choose a progress maximum greater than zero.")
		}
		converted = map[string]any{"current": number, "max": json.Number(strconv.FormatFloat(maximum, 'f', -1, 64))}
	default:
		return nil, fmt.Errorf("Conversion to %s is not supported.", target.FieldType)
	}
	encoded, err := json.Marshal(converted)
	if err != nil {
		return nil, fmt.Errorf("Could not encode converted value.")
	}
	validated, _, _, err := validateFieldValue(target, encoded)
	if err != nil {
		return nil, fmt.Errorf("Converted value is invalid: %s", err.Error())
	}
	return validated, nil
}

func numericType(fieldType string) bool {
	return fieldType == "integer" || fieldType == "decimal" || fieldType == "percentage"
}

func policyString(policy map[string]json.RawMessage, key string) string {
	var value string
	_ = json.Unmarshal(policy[key], &value)
	return value
}

func policyBool(policy map[string]json.RawMessage, key string) bool {
	var value bool
	_ = json.Unmarshal(policy[key], &value)
	return value
}

func policyStringMap(policy map[string]json.RawMessage, key string) map[string]string {
	var result map[string]string
	_ = json.Unmarshal(policy[key], &result)
	return result
}

func policyNumber(policy map[string]json.RawMessage, key string) (float64, error) {
	var number json.Number
	decoder := json.NewDecoder(strings.NewReader(string(policy[key])))
	decoder.UseNumber()
	if err := decoder.Decode(&number); err != nil {
		return 0, fmt.Errorf("Choose a numeric %s value.", key)
	}
	parsed, err := strconv.ParseFloat(string(number), 64)
	if err != nil {
		return 0, err
	}
	return parsed, nil
}

func progressNumber(value any, targetType string, policy map[string]json.RawMessage) (json.Number, error) {
	progress, ok := value.(map[string]any)
	if !ok {
		return "", fmt.Errorf("Progress value must contain current and max numbers.")
	}
	current, cok := progress["current"].(json.Number)
	maximum, mok := progress["max"].(json.Number)
	if !cok || !mok {
		return "", fmt.Errorf("Progress value must contain current and max numbers.")
	}
	currentFloat, err1 := strconv.ParseFloat(string(current), 64)
	maximumFloat, err2 := strconv.ParseFloat(string(maximum), 64)
	if err1 != nil || err2 != nil || maximumFloat <= 0 {
		return "", fmt.Errorf("Progress has an invalid maximum or numeric value.")
	}
	choice := policyString(policy, "progress_value")
	if targetType == "percentage" && choice == "" {
		return "", fmt.Errorf("Choose the explicit current / maximum × 100 strategy for percentage conversion.")
	}
	if targetType == "percentage" && choice != "percentage" {
		return "", fmt.Errorf("A progress field can become a percentage only using current / maximum × 100.")
	}
	var result float64
	switch choice {
	case "current":
		result = currentFloat
	case "maximum":
		result = maximumFloat
	case "percentage":
		result = currentFloat / maximumFloat * 100
	default:
		return "", fmt.Errorf("Choose whether progress should become current, maximum, or percentage.")
	}
	return json.Number(strconv.FormatFloat(result, 'f', -1, 64)), nil
}

func decimalRat(value string) (*big.Rat, bool) {
	if len(value) == 0 || len(value) > 128 || !json.Valid([]byte(value)) {
		return nil, false
	}
	sign := 1
	if value[0] == '-' {
		sign = -1
		value = value[1:]
	}
	exponent := 0
	if index := strings.IndexAny(value, "eE"); index >= 0 {
		parsed, err := strconv.Atoi(value[index+1:])
		if err != nil || parsed < -1000 || parsed > 1000 {
			return nil, false
		}
		exponent = parsed
		value = value[:index]
	}
	places := 0
	if index := strings.IndexByte(value, '.'); index >= 0 {
		places = len(value) - index - 1
		value = value[:index] + value[index+1:]
	}
	numerator, ok := new(big.Int).SetString(value, 10)
	if !ok {
		return nil, false
	}
	if sign < 0 {
		numerator.Neg(numerator)
	}
	scale := places - exponent
	denominator := big.NewInt(1)
	if scale > 0 {
		denominator.Exp(big.NewInt(10), big.NewInt(int64(scale)), nil)
	} else if scale < 0 {
		numerator.Mul(numerator, new(big.Int).Exp(big.NewInt(10), big.NewInt(int64(-scale)), nil))
	}
	return new(big.Rat).SetFrac(numerator, denominator), true
}

func canonicalDecimalText(value string) (string, bool) {
	rat, ok := decimalRat(value)
	if !ok {
		return "", false
	}
	denominator := new(big.Int).Set(rat.Denom())
	two, five, one := big.NewInt(2), big.NewInt(5), big.NewInt(1)
	twos, fives := 0, 0
	for new(big.Int).Mod(new(big.Int).Set(denominator), two).Sign() == 0 {
		denominator.Div(denominator, two)
		twos++
	}
	for new(big.Int).Mod(new(big.Int).Set(denominator), five).Sign() == 0 {
		denominator.Div(denominator, five)
		fives++
	}
	if denominator.Cmp(one) != 0 {
		return "", false
	}
	places := twos
	if fives > places {
		places = fives
	}
	text := rat.FloatString(places)
	if strings.Contains(text, ".") {
		text = strings.TrimRight(strings.TrimRight(text, "0"), ".")
	}
	if text == "-0" || text == "" {
		text = "0"
	}
	return text, true
}

func (a *API) applyFieldTypeMigration(ctx context.Context, tx *sql.Tx, current fieldDefinition, gameID, userID string, input fieldTypeChangeRequest, summary fieldMigrationSummary) error {
	id, err := randomID()
	if err != nil {
		return err
	}
	targetOptions, err := migrationOptions(current, input)
	if err != nil {
		return err
	}
	target := current
	target.FieldType, target.Options = input.TargetType, targetOptions
	if input.DefaultValue.Present {
		if string(input.DefaultValue.Value) == "null" {
			target.DefaultValue = nil
		} else {
			target.DefaultValue = append(json.RawMessage(nil), input.DefaultValue.Value...)
		}
	} else if len(current.DefaultValue) > 0 && string(current.DefaultValue) != "null" {
		target.DefaultValue, err = convertFieldTypeValue(current.FieldType, target, input.Policy, current.DefaultValue)
		if err != nil {
			return err
		}
	}
	policyJSON, err := json.Marshal(input.Policy)
	if err != nil {
		return err
	}
	now := time.Now().UTC().Format(time.RFC3339)
	if _, err := tx.ExecContext(ctx, `INSERT INTO field_type_migrations(id,game_id,field_id,field_name,previous_type,new_type,policy_json,previous_default_json,new_default_json,converted_values,affected_objects,archived_objects_affected,incompatible_values,outcome,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,'applying',?)`, id, gameID, current.ID, current.Name, current.FieldType, target.FieldType, string(policyJSON), nullableJSON(current.DefaultValue), nullableJSON(target.DefaultValue), summary.SavedValues, summary.AffectedObjects, summary.ArchivedObjectsAffected, summary.IncompatibleValues, now); err != nil {
		return err
	}
	if current.FieldType == "formula" && numericType(target.FieldType) {
		if err := a.applyFormulaFieldValues(ctx, tx, current, target, input.Policy, userID, gameID); err != nil {
			return err
		}
	} else if (current.FieldType == "reference" || current.FieldType == "references") && (target.FieldType == "text" || target.FieldType == "long_text") {
		if err := a.applyReferenceFieldToText(ctx, tx, current, target, input.Policy, userID); err != nil {
			return err
		}
	} else if (target.FieldType == "reference" || target.FieldType == "references") && (current.FieldType == "text" || current.FieldType == "long_text" || current.FieldType == "select") {
		if err := a.applyTextToReferenceField(ctx, tx, current, target, input.Policy, userID, gameID); err != nil {
			return err
		}
	} else if err := a.applyStoredFieldValues(ctx, tx, current, target, input.Policy, userID); err != nil {
		return err
	}
	if (len(current.DefaultValue) > 0 && string(current.DefaultValue) != "null" || len(target.DefaultValue) > 0 && string(target.DefaultValue) != "null") && (!rawEqual(current.DefaultValue, target.DefaultValue) || current.FieldType != target.FieldType) {
		if err := a.recordDefaultMigrationEffects(ctx, tx, current, target, userID); err != nil {
			return err
		}
	}
	if current.FieldType == "reference" || current.FieldType == "references" || target.FieldType == "reference" || target.FieldType == "references" {
		if sameMigrationFamily(current.FieldType, target.FieldType) {
			if err := a.recordReferenceMigrationEffects(ctx, tx, current, target, userID); err != nil {
				return err
			}
		}
	}
	if _, err := tx.ExecContext(ctx, `UPDATE field_definitions SET field_type=?,options_json=?,default_json=?,updated_at=? WHERE id=?`, target.FieldType, string(target.Options), nullableJSON(target.DefaultValue), now, current.ID); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `UPDATE field_type_migrations SET outcome='applied' WHERE id=? AND outcome='applying'`, id); err != nil {
		return err
	}
	return nil
}

func (a *API) applyTextToReferenceField(ctx context.Context, tx *sql.Tx, current, target fieldDefinition, policy map[string]json.RawMessage, userID, gameID string) error {
	const batchSize = 300
	lastID := ""
	updatedAt := time.Now().UTC().Format(time.RFC3339)
	for {
		rows, err := tx.QueryContext(ctx, `SELECT o.id,o.name,v.value_json FROM field_values v JOIN objects o ON o.id=v.object_id WHERE v.field_id=? AND o.id>? ORDER BY o.id LIMIT ?`, current.ID, lastID, batchSize)
		if err != nil {
			return err
		}
		type valueRow struct {
			id, name string
			value    []byte
		}
		batch := make([]valueRow, 0, batchSize)
		for rows.Next() {
			var row valueRow
			if err := rows.Scan(&row.id, &row.name, &row.value); err != nil {
				rows.Close()
				return err
			}
			batch = append(batch, row)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		if err := rows.Close(); err != nil {
			return err
		}
		if len(batch) == 0 {
			return nil
		}
		for _, row := range batch {
			converted, err := convertFieldTypeValue(current.FieldType, target, policy, row.value)
			if err != nil || validateMigrationReferenceValue(ctx, tx, gameID, target, converted) != nil {
				return validationError{code: "migration_reference_changed", message: "A mapped object changed or no longer satisfies this field. Review a fresh preview before applying."}
			}
			var refs []string
			if target.FieldType == "reference" {
				var id string
				if err := json.Unmarshal(converted, &id); err != nil {
					return err
				}
				refs = append(refs, id)
			} else if err := json.Unmarshal(converted, &refs); err != nil {
				return err
			}
			for _, refID := range refs {
				if _, err := tx.ExecContext(ctx, `INSERT INTO object_relationships(game_id,object_id,object_type_id,field_id,referenced_object_id) VALUES(?,?,?,?,?)`, gameID, row.id, current.ObjectTypeID, current.ID, refID); err != nil {
					return err
				}
			}
			if err := a.recordFieldMigrationHistory(ctx, tx, current, target, userID, row.id, row.name, row.value, converted, updatedAt); err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `DELETE FROM field_values WHERE object_id=? AND field_id=?`, row.id, current.ID); err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `UPDATE objects SET updated_at=? WHERE id=?`, updatedAt, row.id); err != nil {
				return err
			}
			lastID = row.id
		}
	}
}

func (a *API) applyReferenceFieldToText(ctx context.Context, tx *sql.Tx, current, target fieldDefinition, policy map[string]json.RawMessage, userID string) error {
	if policyString(policy, "relationship_strategy") != "serialize_ids" {
		var count int
		if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM object_relationships WHERE field_id=?`, current.ID).Scan(&count); err != nil {
			return err
		}
		if count > 0 {
			return validationError{code: "relationship_strategy_required", message: "Choose how linked object IDs should be preserved before converting this reference field."}
		}
		return nil
	}
	const batchSize = 300
	lastID := ""
	updatedAt := time.Now().UTC().Format(time.RFC3339)
	for {
		rows, err := tx.QueryContext(ctx, `SELECT o.id,o.name,json_group_array(rel.referenced_object_id) FROM objects o JOIN object_relationships rel ON rel.object_id=o.id WHERE rel.field_id=? AND o.id>? GROUP BY o.id,o.name ORDER BY o.id LIMIT ?`, current.ID, lastID, batchSize)
		if err != nil {
			return err
		}
		type referenceRow struct {
			id, name string
			refs     []byte
		}
		batch := make([]referenceRow, 0, batchSize)
		for rows.Next() {
			var row referenceRow
			if err := rows.Scan(&row.id, &row.name, &row.refs); err != nil {
				rows.Close()
				return err
			}
			batch = append(batch, row)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		if err := rows.Close(); err != nil {
			return err
		}
		if len(batch) == 0 {
			return nil
		}
		for _, row := range batch {
			var refs []string
			if err := json.Unmarshal(row.refs, &refs); err != nil {
				return err
			}
			before, err := convertReferenceFieldValue(current.FieldType, current.FieldType, refs)
			if err != nil {
				return err
			}
			after, err := convertReferenceMigrationValue(current.FieldType, target, policy, refs)
			if err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?) ON CONFLICT(object_id,field_id) DO UPDATE SET value_json=excluded.value_json,updated_at=excluded.updated_at`, row.id, current.ObjectTypeID, current.ID, string(after), updatedAt); err != nil {
				return err
			}
			if err := a.recordFieldMigrationHistory(ctx, tx, current, target, userID, row.id, row.name, before, after, updatedAt); err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `DELETE FROM object_relationships WHERE object_id=? AND field_id=?`, row.id, current.ID); err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `UPDATE objects SET updated_at=? WHERE id=?`, updatedAt, row.id); err != nil {
				return err
			}
			lastID = row.id
		}
	}
}

func (a *API) applyStoredFieldValues(ctx context.Context, tx *sql.Tx, current, target fieldDefinition, policy map[string]json.RawMessage, userID string) error {
	const batchSize = 300
	lastID := ""
	updatedAt := time.Now().UTC().Format(time.RFC3339)
	for {
		rows, err := tx.QueryContext(ctx, `SELECT o.id,o.name,v.value_json FROM field_values v JOIN objects o ON o.id=v.object_id WHERE v.field_id=? AND o.id>? ORDER BY o.id LIMIT ?`, current.ID, lastID, batchSize)
		if err != nil {
			return err
		}
		type storedValue struct {
			objectID, objectName string
			value                []byte
		}
		batch := make([]storedValue, 0, batchSize)
		for rows.Next() {
			var row storedValue
			if err := rows.Scan(&row.objectID, &row.objectName, &row.value); err != nil {
				rows.Close()
				return err
			}
			batch = append(batch, row)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		if err := rows.Close(); err != nil {
			return err
		}
		if len(batch) == 0 {
			return nil
		}
		for _, row := range batch {
			converted, err := convertFieldTypeValue(current.FieldType, target, policy, row.value)
			if err != nil {
				return validationError{code: "migration_value_changed", message: "A value no longer matches the reviewed conversion. Create a new preview and review the affected values."}
			}
			if _, err := tx.ExecContext(ctx, `UPDATE field_values SET value_json=?,updated_at=? WHERE object_id=? AND field_id=?`, string(converted), updatedAt, row.objectID, current.ID); err != nil {
				return err
			}
			if err := a.recordFieldMigrationHistory(ctx, tx, current, target, userID, row.objectID, row.objectName, row.value, converted, updatedAt); err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `UPDATE objects SET updated_at=? WHERE id=?`, updatedAt, row.objectID); err != nil {
				return err
			}
			lastID = row.objectID
		}
	}
}

func (a *API) applyFormulaFieldValues(ctx context.Context, tx *sql.Tx, current, target fieldDefinition, policy map[string]json.RawMessage, userID, gameID string) error {
	const batchSize = 200
	lastID := ""
	updatedAt := time.Now().UTC().Format(time.RFC3339)
	for {
		rows, err := tx.QueryContext(ctx, `SELECT o.id,o.name,o.object_type_id,t.name,o.description,o.image,o.updated_at,o.created_at,o.archived FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.object_type_id=? AND o.id>? ORDER BY o.id LIMIT ?`, current.ObjectTypeID, lastID, batchSize)
		if err != nil {
			return err
		}
		batch := make([]objectRecord, 0, batchSize)
		for rows.Next() {
			item, err := scanObjectRecord(rows, gameID)
			if err != nil {
				rows.Close()
				return err
			}
			batch = append(batch, item)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		if err := rows.Close(); err != nil {
			return err
		}
		if len(batch) == 0 {
			return nil
		}
		if err := a.hydrateObjectsBatchFrom(ctx, tx, batch); err != nil {
			return err
		}
		for _, item := range batch {
			lastID = item.ID
			raw, exists := item.Values[current.ID]
			if item.FormulaErrors[current.ID] != "" || !exists || strings.TrimSpace(string(raw)) == "null" {
				return validationError{code: "formula_freeze_failed", message: "A formula result changed or became unavailable after preview. The migration was rolled back."}
			}
			converted, err := convertFieldTypeValue("decimal", target, policy, raw)
			if err != nil {
				return validationError{code: "formula_freeze_failed", message: "A formula result changed after preview. The migration was rolled back."}
			}
			if _, err := tx.ExecContext(ctx, `INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?) ON CONFLICT(object_id,field_id) DO UPDATE SET value_json=excluded.value_json,updated_at=excluded.updated_at`, item.ID, item.ObjectTypeID, current.ID, string(converted), updatedAt); err != nil {
				return err
			}
			if err := a.recordFieldMigrationHistory(ctx, tx, current, target, userID, item.ID, item.Name, raw, converted, updatedAt); err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `UPDATE objects SET updated_at=? WHERE id=?`, updatedAt, item.ID); err != nil {
				return err
			}
		}
	}
}

func (a *API) recordDefaultMigrationEffects(ctx context.Context, tx *sql.Tx, current, target fieldDefinition, userID string) error {
	const batchSize = 300
	lastID := ""
	updatedAt := time.Now().UTC().Format(time.RFC3339)
	for {
		rows, err := tx.QueryContext(ctx, `SELECT o.id,o.name FROM objects o WHERE o.object_type_id=? AND o.id>? AND NOT EXISTS(SELECT 1 FROM field_values v WHERE v.object_id=o.id AND v.field_id=?) AND NOT EXISTS(SELECT 1 FROM object_relationships rel WHERE rel.object_id=o.id AND rel.field_id=?) ORDER BY o.id LIMIT ?`, current.ObjectTypeID, lastID, current.ID, current.ID, batchSize)
		if err != nil {
			return err
		}
		type objectRow struct{ id, name string }
		batch := make([]objectRow, 0, batchSize)
		for rows.Next() {
			var row objectRow
			if err := rows.Scan(&row.id, &row.name); err != nil {
				rows.Close()
				return err
			}
			batch = append(batch, row)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		if err := rows.Close(); err != nil {
			return err
		}
		if len(batch) == 0 {
			return nil
		}
		for _, row := range batch {
			if err := a.recordFieldMigrationHistory(ctx, tx, current, target, userID, row.id, row.name, current.DefaultValue, target.DefaultValue, updatedAt); err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `UPDATE objects SET updated_at=? WHERE id=?`, updatedAt, row.id); err != nil {
				return err
			}
			lastID = row.id
		}
	}
}

func (a *API) recordReferenceMigrationEffects(ctx context.Context, tx *sql.Tx, current, target fieldDefinition, userID string) error {
	const batchSize = 300
	lastID := ""
	updatedAt := time.Now().UTC().Format(time.RFC3339)
	for {
		rows, err := tx.QueryContext(ctx, `SELECT o.id,o.name,json_group_array(rel.referenced_object_id) FROM objects o JOIN object_relationships rel ON rel.object_id=o.id WHERE rel.field_id=? AND o.id>? GROUP BY o.id,o.name ORDER BY o.id LIMIT ?`, current.ID, lastID, batchSize)
		if err != nil {
			return err
		}
		type referenceRow struct {
			id, name string
			refs     []byte
		}
		batch := make([]referenceRow, 0, batchSize)
		for rows.Next() {
			var row referenceRow
			if err := rows.Scan(&row.id, &row.name, &row.refs); err != nil {
				rows.Close()
				return err
			}
			batch = append(batch, row)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		if err := rows.Close(); err != nil {
			return err
		}
		if len(batch) == 0 {
			return nil
		}
		for _, row := range batch {
			var refs []string
			if err := json.Unmarshal(row.refs, &refs); err != nil {
				return err
			}
			before, err := convertReferenceFieldValue(current.FieldType, current.FieldType, refs)
			if err != nil {
				return err
			}
			after, err := convertReferenceFieldValue(current.FieldType, target.FieldType, refs)
			if err != nil {
				return err
			}
			if err := a.recordFieldMigrationHistory(ctx, tx, current, target, userID, row.id, row.name, before, after, updatedAt); err != nil {
				return err
			}
			if _, err := tx.ExecContext(ctx, `UPDATE objects SET updated_at=? WHERE id=?`, updatedAt, row.id); err != nil {
				return err
			}
			lastID = row.id
		}
	}
}

func (a *API) recordFieldMigrationHistory(ctx context.Context, tx dbExecutor, current, target fieldDefinition, userID, objectID, objectName string, before, after json.RawMessage, at string) error {
	item := objectRecord{objectSummary: objectSummary{ID: objectID, Name: objectName}, GameID: ""}
	if err := tx.QueryRowContext(ctx, `SELECT game_id FROM objects WHERE id=?`, objectID).Scan(&item.GameID); err != nil {
		return err
	}
	change := objectHistoryChange{
		FieldID: current.ID, FieldName: current.Name, FieldType: target.FieldType, BeforeFieldType: current.FieldType,
		Before: append(json.RawMessage(nil), before...), After: append(json.RawMessage(nil), after...),
	}
	return a.writeObjectHistory(ctx, tx, userID, item, "field_type_migrated", "internal_migration", historyTime(time.Now()), []objectHistoryChange{change})
}

func rawEqual(a, b json.RawMessage) bool {
	return strings.TrimSpace(string(a)) == strings.TrimSpace(string(b))
}
