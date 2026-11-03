// Package formula evaluates a deliberately small arithmetic language. Field IDs
// are referenced as @<stable-id>; the language cannot execute code or call functions.
package formula

import (
	"fmt"
	"strconv"
	"strings"
)

type parser struct {
	input  string
	pos    int
	values map[string]float64
}

func Evaluate(expression string, values map[string]float64) (float64, error) {
	p := &parser{input: expression, values: values}
	value, err := p.expression()
	if err != nil {
		return 0, err
	}
	p.space()
	if p.pos != len(p.input) {
		return 0, fmt.Errorf("unexpected character at position %d", p.pos+1)
	}
	return value, nil
}

func References(expression string) []string {
	seen := make(map[string]bool)
	refs := make([]string, 0)
	for i := 0; i < len(expression); i++ {
		if expression[i] != '@' {
			continue
		}
		start := i + 1
		if start < len(expression) && expression[start] == '{' {
			start++
			i = start
			for i < len(expression) && expression[i] != '}' {
				i++
			}
			if i < len(expression) && i > start {
				id := expression[start:i]
				if !seen[id] {
					seen[id] = true
					refs = append(refs, id)
				}
			}
		} else {
			i = start
			for i < len(expression) && !strings.ContainsRune(" \t\r\n+-*/()", rune(expression[i])) {
				i++
			}
			if i > start {
				id := expression[start:i]
				if !seen[id] {
					seen[id] = true
					refs = append(refs, id)
				}
			}
		}
		i--
	}
	return refs
}

func (p *parser) expression() (float64, error) {
	left, err := p.term()
	if err != nil {
		return 0, err
	}
	for {
		p.space()
		if p.take('+') {
			right, e := p.term()
			if e != nil {
				return 0, e
			}
			left += right
			continue
		}
		if p.take('-') {
			right, e := p.term()
			if e != nil {
				return 0, e
			}
			left -= right
			continue
		}
		return left, nil
	}
}
func (p *parser) term() (float64, error) {
	left, err := p.factor()
	if err != nil {
		return 0, err
	}
	for {
		p.space()
		if p.take('*') {
			right, e := p.factor()
			if e != nil {
				return 0, e
			}
			left *= right
			continue
		}
		if p.take('/') {
			right, e := p.factor()
			if e != nil {
				return 0, e
			}
			if right == 0 {
				return 0, fmt.Errorf("division by zero")
			}
			left /= right
			continue
		}
		return left, nil
	}
}
func (p *parser) factor() (float64, error) {
	p.space()
	if p.take('+') {
		return p.factor()
	}
	if p.take('-') {
		v, e := p.factor()
		return -v, e
	}
	if p.take('(') {
		v, e := p.expression()
		if e != nil {
			return 0, e
		}
		p.space()
		if !p.take(')') {
			return 0, fmt.Errorf("expected closing parenthesis")
		}
		return v, nil
	}
	if p.pos >= len(p.input) {
		return 0, fmt.Errorf("expected a number or field reference")
	}
	if p.input[p.pos] == '@' {
		p.pos++
		start := p.pos
		braced := p.pos < len(p.input) && p.input[p.pos] == '{'
		if braced {
			p.pos++
			start = p.pos
			for p.pos < len(p.input) && p.input[p.pos] != '}' {
				p.pos++
			}
		} else {
			for p.pos < len(p.input) && !strings.ContainsRune(" \t\r\n+-*/()", rune(p.input[p.pos])) {
				p.pos++
			}
		}
		if p.pos == start {
			return 0, fmt.Errorf("field reference is missing an ID")
		}
		id := p.input[start:p.pos]
		if braced {
			if p.pos >= len(p.input) {
				return 0, fmt.Errorf("field reference is missing a closing brace")
			}
			p.pos++
		}
		v, ok := p.values[id]
		if !ok {
			return 0, fmt.Errorf("field %s has no numeric value", id)
		}
		return v, nil
	}
	start := p.pos
	for p.pos < len(p.input) && (p.input[p.pos] >= '0' && p.input[p.pos] <= '9' || p.input[p.pos] == '.') {
		p.pos++
	}
	if p.pos == start {
		return 0, fmt.Errorf("unexpected character at position %d", p.pos+1)
	}
	v, err := strconv.ParseFloat(p.input[start:p.pos], 64)
	if err != nil {
		return 0, fmt.Errorf("invalid number")
	}
	return v, nil
}
func (p *parser) space() {
	for p.pos < len(p.input) && (p.input[p.pos] == ' ' || p.input[p.pos] == '\t' || p.input[p.pos] == '\r' || p.input[p.pos] == '\n') {
		p.pos++
	}
}
func (p *parser) take(char byte) bool {
	if p.pos < len(p.input) && p.input[p.pos] == char {
		p.pos++
		return true
	}
	return false
}
