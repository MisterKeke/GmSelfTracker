import { request, setCSRF } from '../api.js';
import { closeModal, ensureDialog } from '../modal.js';
import { esc } from '../utils/formatting.js';

export function bindWorkspaceEvents({ load, render, renderGameList, refreshLibraryResults, refreshObjects, openForm, fieldRow, openGameForm, openSectionForm, openTypeForm, openFieldForm, openObjectForm, renderFieldInput, openAddExisting, openAddExistingGoal, openGoalForm, renderGoalSpecific, addConditionRow, updateConditionRow, openAddTask, openProfile, toast, run, submitForm, state, root }) {
  const findGoal=id=>state.goalCache?.get(id)||state.goalQueryItems?.find(item=>item.id===id)||state.sectionGoals?.find(item=>item.id===id)||state.goals.find(item=>item.id===id);
  root.addEventListener('input',(event)=>{if(event.target.id==='game-search'){state.search=event.target.value;const list=root.querySelector('.game-list');if(list)list.innerHTML=renderGameList();}});
  root.addEventListener('change',(event)=>{
    const action=event.target.dataset.action;
    if(action==='choose-object-type'){const type=state.types.find((item)=>item.id===event.target.value);const form=event.target.closest('form');form.dataset.typeId=type.id;form.querySelector('input[name=type_id]').value=type.id;const fields=form.querySelector('#object-field-inputs');if(fields)fields.innerHTML=(type.fields||[]).filter((field)=>!field.archived).map((field)=>renderFieldInput(field,undefined,null)).join('');}
    if(action==='condition-object'||action==='condition-field'){updateConditionRow(event.target.closest('.goal-condition'),action==='condition-object');}
    if(action==='choose-goal-type'){renderGoalSpecific(event.target.closest('form'),event.target.value);}
    if(action==='toggle-task'){run(async()=>{await request(`/api/goal-tasks/${event.target.dataset.id}`,{method:'PATCH',body:{completed:event.target.checked}});await load();});}
    if(action==='toggle-object-checklist'){const object=state.objects.find((item)=>item.id===event.target.dataset.objectId);const fieldID=event.target.dataset.fieldId;run(async()=>{await request(`/api/objects/${object.id}/values`,{method:'PATCH',body:{values:{[fieldID]:event.target.checked}}});await load();});}
  });
  root.addEventListener('click',(event)=>{
    const button=event.target.closest('[data-action]');if(!button)return;const action=button.dataset.action;const id=button.dataset.id;
    if(action==='reload')window.location.reload();
    else if(action==='new-game')openGameForm(false);
    else if(action==='edit-game')openGameForm(true);
    else if(action==='select-game'){state.game=state.games.find((item)=>item.id===id);state.sections=[];state.types=[];state.objects=[];state.objectCache.clear();state.objectTotal=0;state.goals=[];state.sectionGoals=[];state.goalQueryItems=[];state.goalCache.clear();state.goalStats={total:0,completed:0};state.goalPage=0;state.panel='overview';state.sectionId=null;render();run(load);}
    else if(action==='library-page'){state.objectPage=Math.max(0,Number(button.dataset.page)||0);refreshObjects();}
    else if(action==='section-page'){state.objectPage=Math.max(0,Number(button.dataset.page)||0);refreshObjects();}
    else if(action==='select-panel'){state.panel=button.dataset.panel;state.sectionId=null;state.objectPage=0;render();refreshObjects();}
    else if(action==='select-section'){state.panel='section';state.sectionId=id;state.sectionGoals=[];state.objectPage=0;render();run(load);}
    else if(action==='new-section')openSectionForm();
    else if(action==='edit-section')openSectionForm(state.sections.find((item)=>item.id===id));
    else if(action==='archive-section')run(async()=>{if(!confirm('Archive only this section? Active children stay accessible at the tree root until it is restored. Objects, goals, and all placements remain saved.'))return;await request(`/api/sections/${id}`,{method:'DELETE'});state.sectionId=null;state.panel='overview';await load();toast('Section archived.');});
    else if(action==='new-type')openTypeForm();
    else if(action==='edit-type')openTypeForm(state.types.find((item)=>item.id===id));
    else if(action==='archive-type'||action==='restore-type')run(async()=>{if(action==='archive-type'&&!confirm('Archive this type and hide its objects? Values and placements are preserved.'))return;await request(`/api/object-types/${id}/${action==='archive-type'?'archive':'restore'}`,{method:'POST',body:{}});await load();toast('Type updated.');});
    else if(action==='new-field')openFieldForm(button.dataset.type);
    else if(action==='edit-field'){const field=state.types.flatMap((item)=>item.fields||[]).find((item)=>item.id===id);openFieldForm(field?.object_type_id,field);}
    else if(action==='archive-field')run(async()=>{if(!confirm('Archive this field? Existing values will be preserved.'))return;await request(`/api/fields/${id}/archive`,{method:'POST',body:{}});await load();toast('Field archived.');});
    else if(action==='restore-field')run(async()=>{await request(`/api/fields/${id}/restore`,{method:'POST',body:{}});await load();toast('Field restored.');});
    else if(action==='new-object')openObjectForm();
    else if(action==='edit-object'){const object=state.objects.find((item)=>item.id===id);if(object)openObjectForm(object);else run(async()=>openObjectForm(await request(`/api/objects/${encodeURIComponent(id)}`)));}
    else if(action==='archive-object')run(async()=>{if(!confirm('Archive this object? It will be hidden from normal views and can be restored later.'))return;await request(`/api/objects/${id}/archive`,{method:'POST',body:{}});await load();toast('Object archived.');});
    else if(action==='add-existing')openAddExisting();
    else if(action==='remove-placement')run(async()=>{if(!confirm('Remove this object from the current section? The object itself stays in the game.'))return;await request(`/api/sections/${state.sectionId}/placements/${id}`,{method:'DELETE'});await load();toast('Object removed from this section.');});
    else if(action==='add-existing-goal')openAddExistingGoal();
    else if(action==='remove-goal-placement')run(async()=>{await request(`/api/sections/${state.sectionId}/goals/${id}`,{method:'DELETE'});await load();});
    else if(action==='add-condition')addConditionRow(button.closest('form'));
    else if(action==='remove-condition')button.closest('.goal-condition').remove();
    else if(action==='new-goal')openGoalForm();
    else if(action==='edit-goal')run(async()=>{let goal=findGoal(id);if(!goal){goal=await request(`/api/goals/${encodeURIComponent(id)}`);state.goalCache.set(goal.id,goal);}openGoalForm(goal);});
    else if(action==='open-goals'){state.panel='goals';render();}
    else if(action==='toggle-goal')run(async()=>{const goal=findGoal(id)||await request(`/api/goals/${encodeURIComponent(id)}`);await request(`/api/goals/${id}`,{method:'PATCH',body:{completed:!goal.completed}});await load();});
    else if(action==='edit-goal-progress')run(async()=>{const goal=findGoal(id)||await request(`/api/goals/${encodeURIComponent(id)}`);openForm('UPDATE GOAL PROGRESS',`${fieldRow('Current progress','current',goal.current,'number',true,'min="0" step="any" required')}<input type="hidden" name="goal_id" value="${esc(id)}">`,'UPDATE');ensureDialog(root).querySelector('#modal-form').dataset.form='edit-goal-progress';});
    else if(action==='delete-goal')run(async()=>{if(!confirm('Delete this goal and its checklist tasks?'))return;await request(`/api/goals/${id}`,{method:'DELETE'});await load();toast('Goal deleted.');});
    else if(action==='add-task')run(async()=>{const goal=findGoal(id)||await request(`/api/goals/${encodeURIComponent(id)}`);openAddTask(goal);});
    else if(action==='archive-game')run(async()=>{if(!confirm(`Archive ${state.game.name}? Your data remains saved.`))return;await request(`/api/games/${state.game.id}/archive`,{method:'POST',body:{}});await load();closeModal(ensureDialog(root));toast('Game archived.');});
    else if(action==='restore-game')run(async()=>{await request(`/api/games/${id}/restore`,{method:'POST',body:{}});await load();await openProfile();toast('Game restored.');});
    else if(action==='restore-section')run(async()=>{await request(`/api/sections/${id}/restore`,{method:'POST',body:{}});await load();await openProfile();toast('Section restored.');});
    else if(action==='restore-object')run(async()=>{await request(`/api/objects/${id}/restore`,{method:'POST',body:{}});await load();await openProfile();toast('Object restored.');});
    else if(action==='logout')run(async()=>{await request('/api/auth/logout',{method:'POST'});setCSRF('');window.location.reload();});
    else if(action==='delete-image')run(async()=>{if(!confirm('Delete this uploaded image?'))return;const result=await request(`/api/assets/${id}`,{method:'DELETE'});await openProfile();toast(result?.pending_deletion?'Deletion scheduled; cleanup will retry.':'Image deleted.');});
    else if(action==='profile')run(openProfile);
  });
  root.addEventListener('submit',async(event)=>{
    const form=event.target;if(form.id!=='modal-form')return;event.preventDefault();
    if(form.dataset.saving==='true')return;
    form.dataset.saving='true';form.inert=true;
    const dialog=ensureDialog(root);dialog.setAttribute('aria-busy','true');
    const errorRegion=dialog.querySelector('.form-error');if(errorRegion)errorRegion.hidden=true;
    const closeButtons=[...dialog.querySelectorAll('[data-close]')];closeButtons.forEach(button=>button.disabled=true);
    const submit=event.submitter||ensureDialog(root).querySelector('button[type="submit"][form="modal-form"]');
    const label=submit?.textContent;if(submit){submit.disabled=true;submit.textContent='SAVING…';}
    try{await submitForm(form);}catch(error){toast(error.message,true);if(errorRegion){errorRegion.textContent=error.message;errorRegion.hidden=false;}}finally{delete form.dataset.saving;form.inert=false;dialog.removeAttribute('aria-busy');closeButtons.forEach(button=>button.disabled=false);if(submit?.isConnected){submit.disabled=false;submit.textContent=label;}}
  });

}
