import { request } from '../api.js';
import { closeModal, ensureDialog } from '../modal.js';
import { sectionDisplayConfig, readSectionDisplayConfig, readDefaultValue } from '../utils/formatting.js';

export function createActions({ readGoalTasks, load, render, readValues, readGoalConditions, toast, state, root }) {
  async function submitForm(form) {
    const data=new FormData(form);const action=form.dataset.form;const values=Object.fromEntries(data.entries());
    const iconFile=data.get('icon_file');if(iconFile?.size)values.icon=await uploadImage(iconFile,state.game?.id);
    if(action==='create-game'||action==='edit-game'){
      let cover=values.cover_image||'';const file=data.get('cover_file');if(file?.size){cover=await uploadImage(file,null);}
      const payload={name:values.name,icon:values.icon,description:values.description,personal_notes:values.personal_notes,cover_image:cover};
      if(action==='create-game'){const created=await request('/api/games',{method:'POST',body:payload});state.game=created;state.panel='overview';await load();}
      else{await request(`/api/games/${state.game.id}`,{method:'PATCH',body:payload});await load();}
    } else if(action==='move-section'){
      await request(`/api/sections/${values.section_id}/move`,{method:'POST',body:{parent_id:values.parent_id||null,before_id:values.before_id||''}});if(values.parent_id)state.expanded.add(values.parent_id);await load();
    } else if(action==='create-section'){
      const created=await request(`/api/games/${state.game.id}/sections`,{method:'POST',body:{name:values.name,icon:values.icon,parent_id:values.parent_id||null,display_style:values.display_style,display_config:readSectionDisplayConfig(form)}});if(values.parent_id)state.expanded.add(values.parent_id);await load();state.panel='section';state.sectionId=created.id;render();
    } else if(action==='edit-section'){
      await request(`/api/sections/${values.section_id}`,{method:'PATCH',body:{name:values.name,icon:values.icon,parent_id:values.parent_id||null,display_style:values.display_style,display_config:{...sectionDisplayConfig(state.sections.find(item=>item.id===values.section_id)),...readSectionDisplayConfig(form)},sort_order:Number(values.sort_order||0)}});await load();
    } else if(action==='create-type'||action==='edit-type'){
      const existing=state.types.find((item)=>item.id===values.object_type_id);const file=data.get('type_image_file');
      const image=file?.size?await uploadImage(file,state.game.id):(existing?.image||'');
      await request(existing?`/api/object-types/${existing.id}`:`/api/games/${state.game.id}/object-types`,{method:existing?'PATCH':'POST',body:{name:values.name,icon:values.icon,description:values.description,image,...(existing?{primary_display_field_id:values.primary_display_field_id}:{})}});await load();state.panel='schema';render();
    } else if(action==='create-field'||action==='edit-field'){
      const fieldType=values.field_type;let options={};if(fieldType==='select')options.choices=(values.choices||'').split('\n').map((item)=>item.trim()).filter(Boolean);if(fieldType==='reference'||fieldType==='references'){if(values.allowed_type_id)options.allowed_type_id=values.allowed_type_id;}if(fieldType==='formula'){options.expression=values.expression;if(values.aggregation_relationship)options.aggregation={relationship_field_id:values.aggregation_relationship,field_id:values.aggregation_field||'',operation:values.aggregation_operation};}
      const payload={name:values.name,field_type:fieldType,icon:values.icon,description:values.description,options};if(!['formula','reference','references'].includes(fieldType))payload.default_value=values.default_value===''?null:readDefaultValue(values.default_value,fieldType);
      if(action==='edit-field'){payload.choice_replacements=Object.fromEntries([...form.querySelectorAll('[data-replace-choice]')].filter((select)=>select.value).map((select)=>[select.dataset.replaceChoice,select.value]));payload.remove_incompatible_references=Boolean(form.elements.namedItem('remove_incompatible_references')?.checked);}
      if(action==='create-field')await request(`/api/object-types/${values.object_type_id}/fields`,{method:'POST',body:{...payload,sort_order:values.sort_order?Number(values.sort_order):undefined}});
      else await request(`/api/fields/${values.field_id}`,{method:'PATCH',body:{...payload,field_type:fieldType}});
      await load();state.panel='schema';render();
    } else if(action==='create-object'){
      const type=state.types.find((item)=>item.id===values.object_type_id);const file=data.get('image_file');const image=file?.size?await uploadImage(file,state.game.id):'';const payload={object_type_id:values.object_type_id,name:values.name,description:values.description,image,section_id:values.section_id||'',values:readValues(form,type)};await request(`/api/games/${state.game.id}/objects`,{method:'POST',body:payload});await load();
    } else if(action==='edit-object'){
      const object=state.objects.find((item)=>item.id===values.object_id);const type=state.types.find((item)=>item.id===object.object_type_id);const file=data.get('image_file');const image=file?.size?await uploadImage(file,state.game.id):object.image;await request(`/api/objects/${object.id}`,{method:'PATCH',body:{name:values.name,description:values.description,image,values:readValues(form,type)}});await load();
    } else if(action==='add-placement'){
      await request(`/api/sections/${state.sectionId}/placements`,{method:'POST',body:{object_id:values.object_id}});await load();
    } else if(action==='add-goal-placement'){
      await request(`/api/sections/${state.sectionId}/goals`,{method:'POST',body:{goal_id:values.goal_id}});await load();
    } else if(action==='create-goal'||action==='edit-goal'){
      const existing=state.goals.find(item=>item.id===values.goal_id);
      const goalType=existing?.goal_type||values.goal_type;
      const payload={title:values.title,description:values.description};
      if(goalType==='manual')payload.completed=Boolean(form.elements.completed?.checked);
      if(goalType==='numeric'){payload.current=Number(values.current);payload.target=Number(values.target);}
      if(goalType==='checklist')payload.tasks=readGoalTasks(form);
      if(goalType==='field_target'&&(!existing||form.dataset.conditionsChanged==='true')){payload.conditions=readGoalConditions(form);payload.mode=values.condition_mode;}
      if(existing)await request(`/api/goals/${existing.id}`,{method:'PATCH',body:payload});
      else {
        if(payload.tasks){payload.task_details=payload.tasks.map(task=>({title:task.title,completed:task.completed}));delete payload.tasks;}
        await request(`/api/games/${state.game.id}/goals`,{method:'POST',body:{...payload,goal_type:goalType}});
      }
      await load();if(state.panel!=='section')state.panel='goals';render();
    } else if(action==='edit-goal-progress'){
      const goal=state.goals.find((item)=>item.id===values.goal_id);await request(`/api/goals/${goal.id}`,{method:'PATCH',body:{current:Number(values.current)}});await load();
    } else if(action==='add-task'){
      await request(`/api/goals/${values.goal_id}/tasks`,{method:'POST',body:{title:values.title}});await load();
    } else if(action==='change-password'){
      await request('/api/auth/password',{method:'POST',body:{current_password:values.current_password,password:values.password,confirm_password:values.confirm_password}});toast('Password updated.');
    }
    closeModal(ensureDialog(root));toast('Saved successfully.');
  }

  async function uploadImage(file,gameID) {
    if(file.size>5*1024*1024)throw new Error('Image must be smaller than 5 MB.');const form=new FormData();form.append('file',file);if(gameID)form.append('game_id',gameID);const uploaded=await request('/api/assets',{method:'POST',body:form});return uploaded.url;
  }

  return { submitForm, uploadImage };
}
