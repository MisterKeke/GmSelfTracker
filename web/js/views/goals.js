import { typeLabel, formatNumber, esc } from '../utils/formatting.js';

export function createGoalViews({ state }) {
  function renderGoals() {
    return `<div class="content-heading"><div><h2>◎ GOALS & DEVELOPMENT PLANS</h2><p>Manual milestones, task checklists, numeric progress, and live field targets.</p></div><button class="pixel-button gold" type="button" data-action="new-goal">＋ Add Goal</button></div>
      ${state.goals.length ? `<div class="goal-list light-panel">${state.goals.map((goal) => renderGoal(goal,true)).join('')}</div>` : `<div class="empty-state"><div class="empty-icon">🏁</div><h3>EVERY QUEST STARTS SOMEWHERE</h3><p>Create a manual goal, a checklist of tasks, a numeric tracker, or a goal linked to one of your object fields.</p><button class="pixel-button gold" type="button" data-action="new-goal">＋ Create Your First Goal</button></div>`}`;
  }

  function renderGoal(goal, large = false) {
    const typeLabel = {manual:'Manual goal',checklist:'Checklist',field_target:'Linked field target',numeric:'Numeric progress'}[goal.goal_type] || 'Goal';
    const target = goal.target ? goal.target : 0;
    const current = goal.current || 0;
    const statusText = target?.conditions ? `${current} / ${target.conditions.length} conditions · ${target.mode==='any'?'any':'all'} required` : goal.goal_type === 'field_target' || goal.goal_type === 'numeric' ? `${formatNumber(current)} / ${formatNumber(Number(target) || 0)}` : goal.goal_type === 'checklist' ? `${goal.tasks?.filter((task)=>task.completed).length || 0} / ${goal.tasks?.length || 0} tasks` : goal.completed ? 'Complete' : 'In progress';
    const width = Math.max(0,Math.min(100,Number(goal.progress)||0));
    let controls = '';
    if (goal.goal_type === 'manual') controls = `<button type="button" class="pixel-button small ${goal.completed?'soft':'gold'}" data-action="toggle-goal" data-id="${esc(goal.id)}">${goal.completed?'Mark active':'✓ Mark done'}</button>`;
    if (goal.goal_type === 'numeric') controls = `<button type="button" class="pixel-button small" data-action="edit-goal-progress" data-id="${esc(goal.id)}">Update progress</button>`;
    if (goal.goal_type === 'checklist' && large) controls = `<button type="button" class="pixel-button small" data-action="add-task" data-id="${esc(goal.id)}">＋ Task</button>`;
    return `<article class="goal-card"><div class="goal-top"><span aria-hidden="true">${goal.completed?'✅':goal.goal_type==='field_target'?'✧':goal.goal_type==='checklist'?'▣':'🏁'}</span><span class="goal-title">${esc(goal.title)}</span><span class="goal-state">${goal.completed?'✓':''}</span></div>
      ${goal.description ? `<p style="margin:4px 0;color:#705b70;font-size:12px">${esc(goal.description)}</p>` : ''}
      <div class="progress-meta"><span>${esc(typeLabel)}</span><span>${esc(statusText)} · ${Math.round(width)}%</span></div><div class="progress-track" role="progressbar" aria-valuenow="${Math.round(width)}" aria-valuemin="0" aria-valuemax="100" aria-label="${esc(goal.title)} progress"><div class="progress-fill ${goal.completed?'done':''}" style="width:${width}%"></div></div>
      ${goal.goal_type==='checklist' && large ? `<div class="task-list">${(goal.tasks||[]).map(task=>`<label class="task-line"><input type="checkbox" data-action="toggle-task" data-id="${esc(task.id)}" ${task.completed?'checked':''}><span>${esc(task.title)}</span></label>`).join('')}</div>` : ''}
      <div class="object-actions"><button type="button" class="pixel-button small" data-action="edit-goal" data-id="${esc(goal.id)}">Edit</button>${controls}${state.panel==='section'?`<button type="button" class="pixel-button small soft" data-action="remove-goal-placement" data-id="${esc(goal.id)}">Remove here</button>`:''}<button type="button" class="pixel-button small soft" data-action="delete-goal" data-id="${esc(goal.id)}">Delete</button></div></article>`;
  }

  return { renderGoals, renderGoal };
}
