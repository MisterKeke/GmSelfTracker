import { ensureDialog } from '../modal.js';
import { esc } from '../utils/formatting.js';

export function createGoalEditor({ openForm, fieldRow, toast, state, root }) {
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
    row.innerHTML=`<label>Object<select name="condition_object" data-action="condition-object" required><option value="">Choose object</option>${objects.map((object)=>`<option value="${esc(object.id)}">${esc(object.name)}</option>`).join('')}</select></label><label>Field<select name="condition_field" data-action="condition-field" required></select></label><label>Comparison<select name="condition_operator" required></select></label><label>Target<span data-condition-target></span></label><button type="button" class="pixel-button small soft" data-action="remove-condition">Remove condition</button>`;
    container.append(row);
    const objectSelect=row.querySelector('[name=condition_object]');
    if(condition){if(!objects.some(object=>object.id===condition.object_id))objectSelect.insertAdjacentHTML('beforeend',`<option value="${esc(condition.object_id)}">Unavailable object (preserved)</option>`);objectSelect.value=condition.object_id;}
    updateConditionRow(row,true);
    if(condition){
      const fieldSelect=row.querySelector('[name=condition_field]');
      if(![...fieldSelect.options].some(option=>option.value===condition.field_id))fieldSelect.insertAdjacentHTML('beforeend',`<option value="${esc(condition.field_id)}">Unavailable field (preserved)</option>`);
      fieldSelect.value=condition.field_id;updateConditionRow(row);
      row.querySelector('[name=condition_operator]').value=condition.operator;
      row.querySelector('[name=condition_value]').value=String(condition.value??'');
    }
  }

  function updateConditionRow(row,changedObject=false) {
    const object=state.objects.find((item)=>item.id===row.querySelector('[name=condition_object]').value);
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
      const field=state.objects.find((item)=>item.id===object_id)?.fields?.find((item)=>item.id===field_id);
      if(!field)throw new Error('Choose an object and field for every condition.');
      let value=row.querySelector('[name=condition_value]').value;
      if(['integer','decimal','percentage','progress','formula'].includes(field.field_type))value=Number(value);
      if(field.field_type==='boolean')value=value==='true';
      return {object_id,field_id,operator:row.querySelector('[name=condition_operator]').value,value};
    });
  }

  function openAddTask(goal) { openForm('ADD A CHECKLIST TASK',`${fieldRow('Task name','title','','text',true,'required maxlength="160"')}<input type="hidden" name="goal_id" value="${esc(goal.id)}">`,'ADD TASK');ensureDialog(root).querySelector('#modal-form').dataset.form='add-task'; }

  return { openGoalForm, renderGoalSpecific, addConditionRow, updateConditionRow, readGoalConditions, readGoalTasks, openAddTask };
}
