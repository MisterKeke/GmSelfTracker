package formula

import "testing"

func TestEvaluateArithmeticWithStableFieldIDs(t *testing.T) {
	value, err := Evaluate("(@{field-one} + 4) * 3 - 6 / 2", map[string]float64{"field-one": 10})
	if err != nil {
		t.Fatal(err)
	}
	if value != 39 {
		t.Fatalf("value = %v, want 39", value)
	}
}

func TestEvaluateRejectsUnsafeOrInvalidExpressions(t *testing.T) {
	for _, expression := range []string{"1/0", "1 + unknown()", "@{missing}"} {
		if _, err := Evaluate(expression, map[string]float64{}); err == nil {
			t.Errorf("Evaluate(%q) unexpectedly succeeded", expression)
		}
	}
}

func TestReferencesReturnsStableIDsOnce(t *testing.T) {
	got := References("@{field-one} + @{field-two} + @{field-one}")
	if len(got) != 2 || got[0] != "field-one" || got[1] != "field-two" {
		t.Fatalf("references = %#v", got)
	}
}
