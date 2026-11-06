import { ensureDialog } from '../modal.js';
import { esc } from '../utils/formatting.js';
import { request } from '../api.js';

export function createGoalEditor({ openForm, fieldRow, toast, state, root }) {
  const conditionSearchTimers=new WeakMap();
  const objectByID=id=>state.objectCache?.get(id)||state.objects.find(item=>item.id===id);
  function openGoalForm(existing=null) {
    const styles=[['manual','Manual goal'],['checklist','Checklist'],['numeric','Numeric progress'],['field_target','Rules based on object fields']];
    const body=`${fieldRow('Goal title','title',existing?.title||'','text',true,'required maxlength="160"')}${fieldRow('Description (optional)','description',existing?.description||'','text',true,'maxlength="500"')}<label class="form-field wide">Goal style<select name="goal_type" data-action="choose-goal-type" ${existing?'disabled':''}>${styles.map(([id,label])=>`<option value="${id}" ${(existing?.goal_type||'manual')===id?'selected':''}>${label}</option>`).join('')}</select>${existing?'<small>The goal style is fixed to preserve its data and links.</small>':''}</label><div class="form-field wide" id="goal-specific-fields"></div>${existing?`<input type="hidden" name="goal_id" value="${esc(existing.id)}"><input type="hidden" name="goal_type" value="${esc(existing.goal_type)}">`:''}`;
    openForm(existing?'EDIT GOAL':'CREATE A GOAL',body,existing?'SAVE GOAL':'CREATE GOAL');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=existing?'edit-goal':'create-goal';
    renderGoalSpecific(form,existing?.goal_type||'manual',existing);
  }

  function renderGoalSpecific(form,goalType,existing=null) {
    const target=form.querySelector('#goal-specific-fields');if(!target)return;
    if(goalType==='checklist') {
      target.innerHTML='<div data-task-editor></div><button type="button" class="pixel-button small" data-action="editor-add-task">＋ Task</button><small>Use the arrows to reorder tasks. Existing task IDs are preserved.</small>';
      for(const task of existing?.tasks||[{title:'',completed:false}])addTaskEditorRow(form,task);
    } else if(goalType==='numeric')target.innerHTML=`${fieldRow('Current progress','current',existing?.current??0,'number',false,'required min="0" step="any"')}${fieldRow('Target','target',existing?.target??100,'number',false,'required min="0.000001" step="any"')}`;
    else if(goalType==='field_target') {
      target.innerHTML=`<label class="form-field wide">Complete when<select name="condition_mode"><option value="all">All conditions match</option><option value="any">Any condition matches</option></select></label><div class="form-field wide" id="goal-conditions"></div><button class="pixel-button small" type="button" data-action="add-condition">＋ Condition</button><small class="dialog-help wide">Progress follows object values and cannot be overwritten. Missing or archived values do not match.</small>`;
      const rule=existing?.target?.conditions?existing.target:existing?.object_id?{mode:'all',conditions:[{object_id:existing.object_id,field_id:existing.field_id,operator:'gte',value:existing.target}]}:null;
      form.elements.condition_mode.value=rule?.mode||'all';
      for(const condition of rule?.conditions||[null])addConditionRow(form,condition);
      form.addEventListener('input',event=>{if(event.target.closest('.goal-condition')||event.target.name==='condition_mode')form.dataset.conditionsChanged='true';});
      form.addEventListener('change',event=>{if(event.target.closest('.goal-condition')||event.target.name==='condition_mode')form.dataset.conditionsChanged='true';});
    } else target.innerHTML=`<label class="task-line"><input type="checkbox" name="completed" ${existing?.completed?'checked':''}>Completed</label>`;
  }

  function addTaskEditorRow(form,task={}) {
    const container=form.querySelector('[data-task-editor]');if(!container)return;
    if(container.children.length>=200){toast('Use no more than 200 tasks.',true);return;}
    const row=document.createElement('div');row.className='checklist-editor-row';row.dataset.taskId=task.id||'';
    row.innerHTML=`<input type="hidden" name="task_id" value="${esc(task.id||'')}"><input type="checkbox" name="task_completed" aria-label="Task completed" ${task.completed?'checked':''}><label>Task name<input name="task_title" value="${esc(task.title||'')}" required maxlength="160"></label><button type="button" class="pixel-button small" data-task-move="up" aria-label="Move task up">↑</button><button type="button" class="pixel-button small" data-task-move="down" aria-label="Move task down">↓</button><button type="button" class="pixel-button small soft" data-task-remove aria-label="Remove task">×</button>`;
    container.append(row);
  }

  function readGoalTasks(form) {
    const tasks=[...form.querySelectorAll('.checklist-editor-row')].map(row=>({id:row.dataset.taskId,title:row.querySelector('[name=task_title]').value.trim(),completed:row.querySelector('[name=task_completed]').checked}));
    if(!tasks.length||tasks.some(task=>!task.title))throw new Error('Add at least one named checklist task.');
    return tasks;
  }

  root.addEventListener('click',event=>{
    const button=event.target.closest('button');if(!button)return;
    const form=button.closest('form');
    if(button.dataset.action==='editor-add-task')addTaskEditorRow(form);
    if(button.hasAttribute('data-task-remove'))button.closest('.checklist-editor-row').remove();
    if(button.dataset.taskMove){const row=button.closest('.checklist-editor-row');if(button.dataset.taskMove==='up'&&row.previousElementSibling)row.previousElementSibling.before(row);else if(button.dataset.taskMove==='down'&&row.nextElementSibling)row.nextElementSibling.after(row);}
    if(['add-condition','remove-condition'].includes(button.dataset.action)&&form)form.dataset.conditionsChanged='true';
  });

  function addConditionRow(form,condition=null) {
    const container=form.querySelector('#goal-conditions');if(!container)return;
    if(container.children.length>=20){toast('Use no more than 20 conditions.',true);return;}
    const objects=state.objects.filter((object)=>(object.fields||[]).some((field)=>!field.archived&&!['reference','references'].includes(field.field_type)));
    const row=document.createElement('div');row.className='goal-condition';
    row.innerHTML=`<label>Search objects<input type="search" data-goal-object-search aria-label="Search objects for a goal condition" placeholder="Find an object…"><small data-goal-object-status role="status">Search all objects in this game.</small></label><label>Object<select name="condition_object" data-action="condition-object" required><option value="">Choose object</option>${objects.map((object)=>`<option value="${esc(object.id)}">${esc(object.name)}</option>`).join('')}</select></label><label>Field<select name="condition_field" data-action="condition-field" required></select></label><label>Comparison<select name="condition_operator" required></select></label><label>Target<span data-condition-target></span></label><button type="button" class="pixel-button small soft" data-action="remove-condition">Remove condition</button>`;
    container.append(row);
    const objectSelect=row.querySelector('[name=condition_object]');
    if(condition){if(!objects.some(object=>object.id===condition.object_id))objectSelect.insertAdjacentHTML('beforeend',`<option value="${esc(condition.object_id)}">Unavailable object (preserved)</option>`);objectSelect.value=condition.object_id;}
    updateConditionRow(row,true);
    if(condition){
      const fieldSelect=row.querySelector('[name=condition_field]');
      if(![...fieldSelect.options].some(option=>option.value===condition.field_id))fieldSelect.insertAdjacentHTML('beforeend',`<option value="${esc(condition.field_id)}">Unavailable field (preserved)</option>`);
      fieldSelect.value=condition.field_id;updateConditionRow(row);
      row.querySelector('[name=condition_operator]').value=condition.operator;
      if(row.querySelector('[name=condition_value]'))row.querySelector('[name=condition_value]').value=String(condition.value??'');
      if(!objectByID(condition.object_id))loadSavedConditionObject(row,condition);
    }
  }

  function updateConditionRow(row,changedObject=false) {
    const object=objectByID(row.querySelector('[name=condition_object]').value);
    const select=row.querySelector('[name=condition_field]');
    if(changedObject)select.innerHTML=`<option value="">Choose field</option>${(object?.fields||[]).filter((field)=>!field.archived&&!['reference','references'].includes(field.field_type)).map((field)=>`<option value="${esc(field.id)}">${esc(field.name)}</option>`).join('')}`;
    const field=object?.fields?.find((item)=>item.id===select.value);const kind=field?.field_type;
    const numeric=['integer','decimal','percentage','progress','formula'].includes(kind);
    const ops=[['eq','equals'],['ne','does not equal']];
    if(numeric||kind==='date')ops.push(['gte','at least / on or after'],['lte','at most / on or before'],['gt','greater than / after'],['lt','less than / before']);
    if(['text','long_text'].includes(kind))ops.push(['contains','contains']);
    row.querySelector('[name=condition_operator]').innerHTML=ops.map(([id,label])=>`<option value="${id}">${label}</option>`).join('');
    const target=row.querySelector('[data-condition-target]');
    if(kind==='boolean')target.innerHTML='<select name="condition_value"><option value="true">Checked</option><option value="false">Unchecked</option></select>';
    else if(kind==='select')target.innerHTML=`<select name="condition_value" required>${(field.options?.choices||[]).map((choice)=>`<option value="${esc(choice)}">${esc(choice)}</option>`).join('')}</select>`;
    else target.innerHTML=`<input name="condition_value" type="${numeric?'number':kind==='date'?'date':'text'}" ${numeric?'step="any"':''} required maxlength="2000">`;
  }

  function readGoalConditions(form) {
    const rows=[...form.querySelectorAll('.goal-condition')];
    if(!rows.length)throw new Error('Add at least one condition.');
    return rows.map((row)=>{
      const object_id=row.querySelector('[name=condition_object]').value;
      const field_id=row.querySelector('[name=condition_field]').value;
      const field=objectByID(object_id)?.fields?.find((item)=>item.id===field_id);
      if(!field)throw new Error('Choose an object and field for every condition.');
      let value=row.querySelector('[name=condition_value]').value;
      if(['integer','decimal','percentage','progress','formula'].includes(field.field_type))value=Number(value);
      if(field.field_type==='boolean')value=value==='true';
      return {object_id,field_id,operator:row.querySelector('[name=condition_operator]').value,value};
    });
  }

  async function loadSavedConditionObject(row,condition){
    try{
      const object=await request(`/api/objects/${encodeURIComponent(condition.object_id)}`);
      if(!row.isConnected)return;
      state.objectCache?.set(object.id,object);
      const select=row.querySelector('[name=condition_object]');
      let option=[...select.options].find(item=>item.value===object.id);
      if(!option){option=new Option(`${object.name} · ${object.type_name}`,object.id);select.add(option);}
      else option.textContent=`${object.name} · ${object.type_name}`;
      select.value=object.id;updateConditionRow(row,true);
      const fieldSelect=row.querySelector('[name=condition_field]');
      if(![...fieldSelect.options].some(item=>item.value===condition.field_id))fieldSelect.add(new Option('Unavailable field (preserved)',condition.field_id));
      fieldSelect.value=condition.field_id;updateConditionRow(row);
      row.querySelector('[name=condition_operator]').value=condition.operator;
      if(row.querySelector('[name=condition_value]'))row.querySelector('[name=condition_value]').value=String(condition.value??'');
      row.querySelector('[data-goal-object-status]').textContent=`Loaded ${object.name}.`;
    }catch(error){if(row.isConnected)row.querySelector('[data-goal-object-status]').textContent=`Saved object is unavailable; the condition is preserved. ${error.message}`;}
  }

  root.addEventListener('input',event=>{
    const input=event.target.closest('[data-goal-object-search]');if(!input)return;
    clearTimeout(conditionSearchTimers.get(input));
    const timer=setTimeout(async()=>{
      if(!state.game)return;
      const row=input.closest('.goal-condition'),select=row.querySelector('[name=condition_object]'),status=row.querySelector('[data-goal-object-status]');
      const version=Number(input.dataset.version||0)+1;input.dataset.version=String(version);
      const params=new URLSearchParams({limit:'30',offset:'0',search:input.value||''});status.textContent='Searching objects…';
      try{
        const result=await request(`/api/games/${encodeURIComponent(state.game.id)}/objects?${params}`);
        if(!input.isConnected||Number(input.dataset.version)!==version)return;
        const selected=select.value,choices=new Map();
        for(const option of select.options)if(option.value&&option.value===selected)choices.set(option.value,option.textContent);
        for(const object of result.items||[]){
          state.objectCache?.set(object.id,object);
          if((object.fields||[]).some(field=>!field.archived&&!['reference','references'].includes(field.field_type)))choices.set(object.id,`${object.name} · ${object.type_name}`);
        }
        select.innerHTML=`<option value="">Choose object</option>${[...choices].map(([id,label])=>`<option value="${esc(id)}">${esc(label)}</option>`).join('')}`;select.value=selected;
        status.textContent=`${result.total} matching objects found. Objects without compatible fields are excluded from the options.`;
      }catch(error){if(input.isConnected&&Number(input.dataset.version)===version)status.textContent=error.message;}
    },250);
    conditionSearchTimers.set(input,timer);
  });

  function openAddTask(goal) { openForm('ADD A CHECKLIST TASK',`${fieldRow('Task name','title','','text',true,'required maxlength="160"')}<input type="hidden" name="goal_id" value="${esc(goal.id)}">`,'ADD TASK');ensureDialog(root).querySelector('#modal-form').dataset.form='add-task'; }

  async function openGoalDependencies(goal) {
    const current=await request(`/api/goals/${encodeURIComponent(goal.id)}/dependencies`);
    const selected=new Set((current.items||[]).map(item=>item.id));
    const choices=new Map((current.items||[]).map(item=>[item.id,`${item.title}${item.completed?' · complete':''}`]));
    openForm(`GOAL DEPENDENCIES · ${goal.title}`,`<p class="dialog-help">Choose goals that must be complete before this goal is ready. Dependencies stay within this game; cycles are rejected.</p><div class="form-field wide"><label for="goal-dependency-search">Find prerequisite goals<input id="goal-dependency-search" type="search" data-dependency-search placeholder="Search goals…"><small data-dependency-status role="status">Search this game’s goals. Select several prerequisites.</small></label></div><div class="form-field wide"><label for="goal-dependency-list">Prerequisite goals</label><select id="goal-dependency-list" name="depends_on_ids" multiple size="9">${[...choices].map(([id,label])=>`<option value="${esc(id)}" selected>${esc(label)}</option>`).join('')}</select><small>Use Ctrl or Command to select multiple goals. You can remove a prerequisite by clearing its selection.</small></div><input type="hidden" name="goal_id" value="${esc(goal.id)}">`,'SAVE DEPENDENCIES');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form='edit-goal-dependencies';
    const search=form.querySelector('[data-dependency-search]'),select=form.elements.depends_on_ids,status=form.querySelector('[data-dependency-status]');
    let version=0,timer;
    const searchGoals=async()=>{
      const thisVersion=++version,params=new URLSearchParams({limit:'30',offset:'0',search:search.value||'',sort:'title',direction:'asc'});status.textContent='Searching goals…';
      try{
        const result=await request(`/api/games/${encodeURIComponent(goal.game_id)}/goals?${params}`);
        if(!search.isConnected||thisVersion!==version)return;
        for(const item of result.items||[])if(item.id!==goal.id)choices.set(item.id,`${item.title}${item.completed?' · complete':''}`);
        const active=[...select.selectedOptions].map(option=>option.value);for(const id of active)selected.add(id);for(const id of [...selected])if(!active.includes(id)&&!choices.has(id))selected.delete(id);
        select.innerHTML=[...choices].filter(([id])=>id!==goal.id).map(([id,label])=>`<option value="${esc(id)}" ${selected.has(id)?'selected':''}>${esc(label)}</option>`).join('');
        status.textContent=`${result.total} matching goals found. Complete a prerequisite to unlock this goal.`;
      }catch(error){if(search.isConnected&&thisVersion===version)status.textContent=error.message;}
    };
    search.addEventListener('input',()=>{clearTimeout(timer);timer=setTimeout(searchGoals,220);});
    select.addEventListener('change',()=>{selected.clear();for(const option of select.selectedOptions)selected.add(option.value);});
    searchGoals();
  }

  return { openGoalForm, renderGoalSpecific, addConditionRow, updateConditionRow, readGoalConditions, readGoalTasks, openAddTask, openGoalDependencies };
}
