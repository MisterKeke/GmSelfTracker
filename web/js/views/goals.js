import { typeLabel, formatNumber, esc } from '../utils/formatting.js';
import { request } from '../api.js';

export function createGoalViews({ state, root }) {
  function renderGoals() {
    const filters=state.goalFilters;
    queueMicrotask(()=>refreshGoalResults());
    return `<div class="content-heading"><div><h2>◎ GOALS & DEVELOPMENT PLANS</h2><p>Manual milestones, task checklists, numeric progress, and live field targets.</p></div><button class="pixel-button gold" type="button" data-action="new-goal">＋ Add Goal</button></div>
      ${state.goals.length?`<section class="goal-browser light-panel" aria-label="Goal search and filters"><div class="library-filters"><label>Search goals<input type="search" data-goal-search value="${esc(filters.search)}" placeholder="Title or description"></label><label>Status<select data-goal-filter="status">${[['all','All'],['active','Active'],['completed','Completed']].map(([id,label])=>`<option value="${id}" ${filters.status===id?'selected':''}>${label}</option>`).join('')}</select></label><label>Type<select data-goal-filter="type">${[['all','All types'],['manual','Manual'],['checklist','Checklist'],['numeric','Numeric'],['field_linked','Field-linked']].map(([id,label])=>`<option value="${id}" ${filters.type===id?'selected':''}>${label}</option>`).join('')}</select></label><label>Sort by<select data-goal-filter="sort">${[['title','Title'],['created_at','Date created'],['updated_at','Date updated'],['progress','Progress'],['completed','Completion status']].map(([id,label])=>`<option value="${id}" ${filters.sort===id?'selected':''}>${label}</option>`).join('')}</select></label><label>Order<select data-goal-filter="direction"><option value="asc" ${filters.direction==='asc'?'selected':''}>Ascending</option><option value="desc" ${filters.direction!=='asc'?'selected':''}>Descending</option></select></label></div></section><div id="goal-results">${renderGoalResults()}</div>`:`<div class="empty-state"><div class="empty-icon">🏁</div><h3>EVERY QUEST STARTS SOMEWHERE</h3><p>Create a manual goal, a checklist of tasks, a numeric tracker, or a goal linked to one of your object fields.</p><button class="pixel-button gold" type="button" data-action="new-goal">＋ Create Your First Goal</button></div>`}`;
  }

  function renderGoalResults(){
    if(state.goalQueryError)return `<p class="form-error" role="alert">${esc(state.goalQueryError)} <button class="pixel-button small" type="button" data-goal-retry>Retry</button></p>`;
    if(state.goalQueryLoading)return '<p class="query-count" role="status">Loading goals…</p>';
    const items=state.goalQueryItems||[],total=Number(state.goalQueryTotal)||0,pages=Math.max(1,Math.ceil(total/20));
    if(!total)return '<div class="empty-state">No goals match these search and filter settings.</div>';
    return `<div class="library-pagination"><span>${total} goals · page ${state.goalPage+1} / ${pages}</span><button class="pixel-button small" type="button" data-goal-page="${state.goalPage-1}" ${state.goalPage===0?'disabled':''}>Previous</button><button class="pixel-button small" type="button" data-goal-page="${state.goalPage+1}" ${state.goalPage===pages-1?'disabled':''}>Next</button></div><div class="goal-list light-panel">${items.map(goal=>renderGoal(goal,true)).join('')}</div>`;
  }

  let searchTimer,querySequence=0,queryController;
  async function refreshGoalResults(){
    if(state.panel!=='goals'||!state.game)return;
    const sequence=++querySequence,params=new URLSearchParams({limit:'20',offset:String(state.goalPage*20),...state.goalFilters});
    queryController?.abort();queryController=new AbortController();
    state.goalQueryLoading=true;state.goalQueryError='';
    const target=root?.querySelector('#goal-results');if(target)target.innerHTML=renderGoalResults();
    try{
      const result=await request(`/api/games/${encodeURIComponent(state.game.id)}/goals?${params}`,{signal:queryController.signal});
      if(sequence!==querySequence||state.panel!=='goals')return;
      state.goalQueryTotal=Number(result.total)||0;
      if(state.goalPage>0&&state.goalPage*20>=state.goalQueryTotal){state.goalPage=Math.max(0,Math.ceil(state.goalQueryTotal/20)-1);return refreshGoalResults();}
      state.goalQueryItems=result.items||[];state.goalQueryLoading=false;
      for(const goal of state.goalQueryItems)state.goalCache.set(goal.id,goal);
    }catch(error){if(sequence===querySequence){state.goalQueryLoading=false;state.goalQueryError=error.message;}}
    const current=root?.querySelector('#goal-results');if(current)current.innerHTML=renderGoalResults();
  }

  root?.addEventListener('input',event=>{
    if(!event.target.matches('[data-goal-search]'))return;
    state.goalFilters.search=event.target.value;state.goalPage=0;clearTimeout(searchTimer);searchTimer=setTimeout(refreshGoalResults,250);
  });
  root?.addEventListener('change',event=>{
    const select=event.target.closest('[data-goal-filter]');if(!select)return;
    state.goalFilters[select.dataset.goalFilter]=select.value;state.goalPage=0;refreshGoalResults();
  });
  root?.addEventListener('click',event=>{
    const button=event.target.closest('[data-goal-page],[data-goal-retry]');if(!button)return;
    if(button.hasAttribute('data-goal-page'))state.goalPage=Math.max(0,Number(button.dataset.goalPage)||0);refreshGoalResults();
  });

  function renderGoal(goal, large = false) {
    const typeLabel = {manual:'Manual goal',checklist:'Checklist',field_target:'Linked field target',numeric:'Numeric progress'}[goal.goal_type] || 'Goal';
    const target = goal.target ? goal.target : 0;
    const current = goal.current || 0;
    const statusText = target?.conditions ? `${current} / ${target.conditions.length} conditions · ${target.mode==='any'?'any':'all'} required` : goal.goal_type === 'field_target' || goal.goal_type === 'numeric' ? `${formatNumber(current)} / ${formatNumber(Number(target) || 0)}` : goal.goal_type === 'checklist' ? `${goal.tasks?.filter((task)=>task.completed).length || 0} / ${goal.tasks?.length || 0} tasks` : goal.completed ? 'Complete' : 'In progress';
    const width = Math.max(0,Math.min(100,Number(goal.progress)||0));
    let controls = '';
    if (goal.goal_type === 'manual') controls = `<button type="button" class="pixel-button small ${goal.completed?'soft':'gold'}" data-action="toggle-goal" data-id="${esc(goal.id)}" ${!goal.completed&&goal.ready===false?'disabled':''}>${goal.completed?'Mark active':goal.ready===false?'Blocked by prerequisite':'✓ Mark done'}</button>`;
    if (goal.goal_type === 'numeric') controls = `<button type="button" class="pixel-button small" data-action="edit-goal-progress" data-id="${esc(goal.id)}">Update progress</button>`;
    if (goal.goal_type === 'checklist' && large) controls = `<button type="button" class="pixel-button small" data-action="add-task" data-id="${esc(goal.id)}">＋ Task</button>`;
    const conditionDetails=goal.condition_details?.length?`<div class="goal-condition-details"><strong>Conditions</strong>${goal.condition_details.map(condition=>{
      const state=condition.state||'not_satisfied';
      const icon=state==='satisfied'?'✓':state==='not_satisfied'?'✗':'?';
      const operator=({eq:'=',ne:'≠',gt:'>',gte:'≥',lt:'<',lte:'≤',contains:'contains'})[condition.operator]||condition.operator;
      return `<div class="goal-condition-status ${state}"><span aria-hidden="true">${icon}</span><span><b>${esc(condition.object_name||'Unavailable object')} · ${esc(condition.field_name||'Unavailable field')}</b><small>${esc(operator)} ${esc(displayConditionValue(condition.target))} · Current: ${esc(displayConditionValue(condition.current_value))}${condition.error?` · ${esc(condition.error)}`:''}</small></span></div>`;
    }).join('')}</div>`:'';
    const dependencyDetails=goal.dependencies?.length?`<div class="goal-dependency-details ${goal.ready===false?'blocked':''}"><strong>${goal.ready===false?'Locked · finish these first':'Ready · prerequisites complete'}</strong><ul>${goal.dependencies.map(dependency=>`<li>${dependency.completed?'✓':'○'} ${esc(dependency.title)}</li>`).join('')}</ul></div>`:'';
    return `<article class="goal-card"><div class="goal-top"><span aria-hidden="true">${goal.completed?'✅':goal.goal_type==='field_target'?'✧':goal.goal_type==='checklist'?'▣':'🏁'}</span><span class="goal-title">${esc(goal.title)}</span><span class="goal-state">${goal.completed?'✓':''}</span></div>
      ${goal.description ? `<p style="margin:4px 0;color:#705b70;font-size:12px">${esc(goal.description)}</p>` : ''}
      <div class="progress-meta"><span>${esc(typeLabel)}</span><span>${esc(statusText)} · ${Math.round(width)}%</span></div><div class="progress-track" role="progressbar" aria-valuenow="${Math.round(width)}" aria-valuemin="0" aria-valuemax="100" aria-label="${esc(goal.title)} progress"><div class="progress-fill ${goal.completed?'done':''}" style="width:${width}%"></div></div>
      ${conditionDetails}${dependencyDetails}
      ${goal.goal_type==='checklist' && large ? `<div class="task-list">${(goal.tasks||[]).map(task=>`<label class="task-line"><input type="checkbox" data-action="toggle-task" data-id="${esc(task.id)}" ${task.completed?'checked':''}><span>${esc(task.title)}</span></label>`).join('')}</div>` : ''}
      <div class="object-actions"><button type="button" class="pixel-button small" data-action="edit-goal" data-id="${esc(goal.id)}">Edit</button><button type="button" class="pixel-button small soft" data-action="goal-dependencies" data-id="${esc(goal.id)}">Dependencies</button>${controls}${state.panel==='section'?`<button type="button" class="pixel-button small soft" data-action="remove-goal-placement" data-id="${esc(goal.id)}">Remove here</button>`:''}<button type="button" class="pixel-button small soft" data-action="delete-goal" data-id="${esc(goal.id)}">Delete</button></div></article>`;
  }

  function displayConditionValue(raw){
    if(raw===undefined||raw===null||raw==='')return '—';
    let value=raw;
    if(typeof raw==='string'){try{value=JSON.parse(raw);}catch{}}
    if(value&&typeof value==='object'&&Number.isFinite(value.current)&&Number.isFinite(value.max))return `${formatNumber(value.current)} / ${formatNumber(value.max)}`;
    if(typeof value==='boolean')return value?'Yes':'No';
    return typeof value==='object'?JSON.stringify(value):String(value);
  }

  return { renderGoals, renderGoal };
}
