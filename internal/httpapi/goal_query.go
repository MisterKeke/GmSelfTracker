package httpapi

import (
	"context"
	"strings"
)

func (a *API) goalTaskSummaries(ctx context.Context, goalIDs []string) (map[string]goalTaskSummary, error) {
	summaries := make(map[string]goalTaskSummary)
	const batchSize = 400
	for start := 0; start < len(goalIDs); start += batchSize {
		end := start + batchSize
		if end > len(goalIDs) {
			end = len(goalIDs)
		}
		batch := goalIDs[start:end]
		placeholders := strings.TrimSuffix(strings.Repeat("?,", len(batch)), ",")
		args := make([]any, len(batch))
		for i := range batch {
			args[i] = batch[i]
		}
		rows, err := a.db.QueryContext(ctx, `SELECT goal_id,COUNT(*),SUM(completed) FROM goal_tasks WHERE goal_id IN (`+placeholders+`) GROUP BY goal_id`, args...)
		if err != nil {
			return nil, err
		}
		for rows.Next() {
			var id string
			var summary goalTaskSummary
			if err := rows.Scan(&id, &summary.total, &summary.completed); err != nil {
				rows.Close()
				return nil, err
			}
			summaries[id] = summary
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return nil, err
		}
		if err := rows.Close(); err != nil {
			return nil, err
		}
	}
	return summaries, nil
}

func (a *API) populateGoalListDetails(ctx context.Context, goals []goal) error {
	if len(goals) == 0 {
		return nil
	}
	indexes := make(map[string]int, len(goals))
	ids := make([]string, len(goals))
	for i := range goals {
		indexes[goals[i].ID] = i
		ids[i] = goals[i].ID
		goals[i].SectionIDs = []string{}
		if goals[i].GoalType == "checklist" {
			goals[i].Tasks = []goalTask{}
		}
	}
	const batchSize = 400
	for start := 0; start < len(ids); start += batchSize {
		end := start + batchSize
		if end > len(ids) {
			end = len(ids)
		}
		batch := ids[start:end]
		placeholders := strings.TrimSuffix(strings.Repeat("?,", len(batch)), ",")
		args := make([]any, len(batch))
		for i := range batch {
			args[i] = batch[i]
		}
		rows, err := a.db.QueryContext(ctx, `SELECT goal_id,id,title,completed,sort_order FROM goal_tasks WHERE goal_id IN (`+placeholders+`) ORDER BY goal_id,sort_order,id`, args...)
		if err != nil {
			return err
		}
		for rows.Next() {
			var goalID string
			var item goalTask
			var completed int
			if err := rows.Scan(&goalID, &item.ID, &item.Title, &completed, &item.SortOrder); err != nil {
				rows.Close()
				return err
			}
			item.Completed = completed != 0
			if index, ok := indexes[goalID]; ok && goals[index].GoalType == "checklist" {
				goals[index].Tasks = append(goals[index].Tasks, item)
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
	for start := 0; start < len(ids); start += batchSize {
		end := start + batchSize
		if end > len(ids) {
			end = len(ids)
		}
		batch := ids[start:end]
		placeholders := strings.TrimSuffix(strings.Repeat("?,", len(batch)), ",")
		args := make([]any, len(batch))
		for i := range batch {
			args[i] = batch[i]
		}
		rows, err := a.db.QueryContext(ctx, `SELECT p.goal_id,p.section_id FROM section_goal_placements p JOIN sections s ON s.id=p.section_id WHERE p.goal_id IN (`+placeholders+`) AND s.archived=0 ORDER BY p.goal_id,s.sort_order,s.id`, args...)
		if err != nil {
			return err
		}
		for rows.Next() {
			var goalID, sectionID string
			if err := rows.Scan(&goalID, &sectionID); err != nil {
				rows.Close()
				return err
			}
			if index, ok := indexes[goalID]; ok {
				goals[index].SectionIDs = append(goals[index].SectionIDs, sectionID)
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
	return nil
}
