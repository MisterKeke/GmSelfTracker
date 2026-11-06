import { request } from '../api.js';
import { closeModal, ensureDialog } from '../modal.js';
import { sectionDisplayConfig, readSectionDisplayConfig, readDefaultValue } from '../utils/formatting.js';
import { upsertDashboardWidget } from '../utils/dashboard-layout.js';

export function createActions({ readGoalTasks, load, render, readValues, readGoalConditions, readDashboardWidget, openObjectGallery, openObjectPresets, migrationRequest, renderMigrationPreview, toast, state, root }) {
  const findGoal=id=>state.goalCache?.get(id)||state.sectionGoals?.find(item=>item.id===id)||state.goalQueryItems?.find(item=>item.id===id)||state.goals.find(item=>item.id===id);
  async function submitForm(form) {
    const data=new FormData(form);const action=form.dataset.form;const values=Object.fromEntries(data.entries());
    const operationUploads=[],operationGalleryEntries=[];
    const uploadForSave=async(file,gameID)=>{const url=await uploadImage(file,gameID);operationUploads.push(url);return url;};
    try {
    const iconFile=data.get('icon_file');if(iconFile?.size)values.icon=await uploadForSave(iconFile,state.game?.id);
    if(action==='dashboard-widget'){
      const widget=readDashboardWidget(form);if(!state.dashboardLayout)state.dashboardLayout={version:1,widgets:[]};upsertDashboardWidget(state.dashboardLayout,widget);
      state.dashboardData.clear();closeModal(ensureDialog(root));render();toast('Widget added to the dashboard draft. Save the dashboard to apply it.');return;
    } else if(action==='edit-object-gallery'){
      const objectID=values.object_id;const itemNodes=[...form.querySelectorAll('[data-gallery-item]')];
      const remaining=itemNodes.filter(node=>!node.querySelector('[data-gallery-remove]')?.checked).length;
      const files=[...(form.elements.namedItem('gallery_files')?.files||[])];
      if(remaining+files.length>50)throw new Error('An object can contain at most 50 gallery images.');
      const primaryID=form.elements.namedItem('primary_gallery_image')?.value||'';
      const uploaded=[];
      for(const file of files){const url=await uploadForSave(file,state.game.id);uploaded.push({url,assetID:url.split('/').pop(),caption:file.name.slice(0,500)});}
      for(const node of itemNodes){const galleryID=node.dataset.galleryItem;if(node.querySelector('[data-gallery-remove]')?.checked){await request(`/api/object-gallery/${encodeURIComponent(galleryID)}`,{method:'DELETE'});continue;}
        const caption=form.elements.namedItem(`gallery_caption_${galleryID}`)?.value||'';const sortOrder=Number(form.elements.namedItem(`gallery_order_${galleryID}`)?.value||0);
        await request(`/api/object-gallery/${encodeURIComponent(galleryID)}`,{method:'PATCH',body:{caption,sort_order:sortOrder,is_primary:galleryID===primaryID}});
      }
      for(const image of uploaded){const result=await request(`/api/objects/${encodeURIComponent(objectID)}/gallery`,{method:'POST',body:{asset_id:image.assetID,caption:image.caption}});operationGalleryEntries.push(result.id);}
      closeModal(ensureDialog(root));await load();await openObjectGallery(objectID);toast('Gallery saved.');return;
    } else if(action==='edit-goal-dependencies'){
      const select=form.elements.namedItem('depends_on_ids');const dependsOnIDs=[...(select?.selectedOptions||[])].map(option=>option.value);
      await request(`/api/goals/${encodeURIComponent(values.goal_id)}/dependencies`,{method:'PUT',body:{depends_on_ids:dependsOnIDs}});await load();render();
    } else if(action==='create-object-preset'||action==='edit-object-preset'){
      const object=state.presetObject;if(!object)throw new Error('The object for this preset is no longer available.');
      const type=state.types.find(item=>item.id===object.object_type_id);if(!type)throw new Error('The object type is no longer available.');
      const allValues=readValues(form,type);const overrideValues={};
      for(const checkbox of form.querySelectorAll('[data-preset-override]:checked'))if(Object.prototype.hasOwnProperty.call(allValues,checkbox.value))overrideValues[checkbox.value]=allValues[checkbox.value];
      const body={name:values.name,description:values.description||'',values:overrideValues};
      if(action==='create-object-preset')await request(`/api/objects/${encodeURIComponent(object.id)}/presets`,{method:'POST',body});
      else await request(`/api/object-presets/${encodeURIComponent(values.preset_id)}`,{method:'PATCH',body});
      closeModal(ensureDialog(root));toast('Preset saved.');await openObjectPresets(object);return;
    } else if(action==='field-migration-preview'||action==='field-migration-apply'){
      const existing=state.types.flatMap(type=>type.fields||[]).find(field=>field.id===values.field_id);
      if(!existing)throw new Error('This field is no longer available. Reload the data types and try again.');
      const {fieldID,payload}=migrationRequest(form,existing);
      if(action==='field-migration-preview'){
        const summary=await request(`/api/fields/${encodeURIComponent(fieldID)}/type-change/preview`,{method:'POST',body:payload});
        renderMigrationPreview(form,summary);
        return;
      }
      try{await request(`/api/fields/${encodeURIComponent(fieldID)}/type-change`,{method:'POST',body:payload});}
      catch(error){
        if(error.code==='stale_preview'){
          delete payload.preview_token;
          const summary=await request(`/api/fields/${encodeURIComponent(fieldID)}/type-change/preview`,{method:'POST',body:payload});
          renderMigrationPreview(form,summary);
          toast('The data changed after preview. Review the updated migration summary.',true);
          return;
        }
        throw error;
      }
      state.panel='schema';await load();
    } else if(action==='create-template'){
      const checked=name=>Boolean(form.elements.namedItem(name)?.checked);
      await request(`/api/games/${encodeURIComponent(state.game.id)}/templates`,{method:'POST',body:{name:values.name,description:values.description||'',icon:values.icon,include_sections:checked('include_sections'),include_types:checked('include_types'),include_field_defaults:checked('include_field_defaults'),include_presentation:checked('include_presentation')}});
      state.panel='templates';await load();render();
    } else if(action==='edit-template'){
      await request(`/api/templates/${encodeURIComponent(values.template_id)}`,{method:'PATCH',body:{name:values.name,description:values.description||'',icon:values.icon}});state.panel='templates';await load();render();
    } else if(action==='import-template'){
      const file=data.get('template_file');if(!file?.size)throw new Error('Choose a template JSON file.');
      let document;try{document=JSON.parse(await file.text());}catch{throw new Error('This file is not valid JSON.');}
      await request('/api/templates/import',{method:'POST',body:document});state.panel='templates';await load();render();
    } else if(action==='create-game'||action==='edit-game'){
      let cover=values.cover_image||'';const file=data.get('cover_file');if(file?.size){cover=await uploadForSave(file,null);}
      const payload={name:values.name,icon:values.icon,description:values.description,personal_notes:values.personal_notes,cover_image:cover};
      if(action==='create-game'){const created=values.template_id?await request(`/api/templates/${encodeURIComponent(values.template_id)}/instantiate`,{method:'POST',body:{name:values.name,icon:values.icon}}):await request('/api/games',{method:'POST',body:payload});state.game=created;state.panel='overview';await load();}
      else{await request(`/api/games/${state.game.id}`,{method:'PATCH',body:payload});await load();}
    } else if(action==='move-section'){
      await request(`/api/sections/${values.section_id}/move`,{method:'POST',body:{parent_id:values.parent_id||null,before_id:values.before_id||''}});if(values.parent_id)state.expanded.add(values.parent_id);await load();
    } else if(action==='create-section'){
      const created=await request(`/api/games/${state.game.id}/sections`,{method:'POST',body:{name:values.name,icon:values.icon,parent_id:values.parent_id||null,display_style:values.display_style,display_config:readSectionDisplayConfig(form)}});if(values.parent_id)state.expanded.add(values.parent_id);await load();state.panel='section';state.sectionId=created.id;render();
    } else if(action==='edit-section'){
      await request(`/api/sections/${values.section_id}`,{method:'PATCH',body:{name:values.name,icon:values.icon,parent_id:values.parent_id||null,display_style:values.display_style,display_config:{...sectionDisplayConfig(state.sections.find(item=>item.id===values.section_id)),...readSectionDisplayConfig(form)},sort_order:Number(values.sort_order||0)}});await load();
    } else if(action==='create-type'||action==='edit-type'){
      const existing=state.types.find((item)=>item.id===values.object_type_id);const file=data.get('type_image_file');
      const image=file?.size?await uploadForSave(file,state.game.id):(existing?.image||'');
      await request(existing?`/api/object-types/${existing.id}`:`/api/games/${state.game.id}/object-types`,{method:existing?'PATCH':'POST',body:{name:values.name,icon:values.icon,description:values.description,image,...(existing?{primary_display_field_id:values.primary_display_field_id}:{})}});await load();state.panel='schema';render();
    } else if(action==='create-field'||action==='edit-field'){
      const fieldType=values.field_type;let options={};if(fieldType==='select')options.choices=(values.choices||'').split('\n').map((item)=>item.trim()).filter(Boolean);if(fieldType==='reference'||fieldType==='references'){if(values.allowed_type_id)options.allowed_type_id=values.allowed_type_id;}if(fieldType==='formula'){options.expression=values.expression;if(values.aggregation_relationship)options.aggregation={relationship_field_id:values.aggregation_relationship,field_id:values.aggregation_field||'',operation:values.aggregation_operation};}
      const payload={name:values.name,field_type:fieldType,icon:values.icon,description:values.description,options};if(!['formula','reference','references'].includes(fieldType))payload.default_value=values.default_value===''?null:readDefaultValue(values.default_value,fieldType);
      if(action==='edit-field'){payload.choice_replacements=Object.fromEntries([...form.querySelectorAll('[data-replace-choice]')].filter((select)=>select.value).map((select)=>[select.dataset.replaceChoice,select.value]));payload.remove_incompatible_references=Boolean(form.elements.namedItem('remove_incompatible_references')?.checked);}
      if(action==='create-field')await request(`/api/object-types/${values.object_type_id}/fields`,{method:'POST',body:{...payload,sort_order:values.sort_order?Number(values.sort_order):undefined}});
      else await request(`/api/fields/${values.field_id}`,{method:'PATCH',body:{...payload,field_type:fieldType}});
      await load();state.panel='schema';render();
    } else if(action==='create-object'){
      const type=state.types.find((item)=>item.id===values.object_type_id);const file=data.get('image_file');const image=file?.size?await uploadForSave(file,state.game.id):'';const payload={object_type_id:values.object_type_id,name:values.name,description:values.description,image,section_id:values.section_id||'',values:readValues(form,type)};await request(`/api/games/${state.game.id}/objects`,{method:'POST',body:payload});await load();
    } else if(action==='edit-object'){
      const object=state.objects.find((item)=>item.id===values.object_id);const type=state.types.find((item)=>item.id===object.object_type_id);const file=data.get('image_file');const image=file?.size?await uploadForSave(file,state.game.id):object.image;await request(`/api/objects/${object.id}`,{method:'PATCH',body:{name:values.name,description:values.description,image,values:readValues(form,type)}});await load();
    } else if(action==='add-placement'){
      await request(`/api/sections/${state.sectionId}/placements`,{method:'POST',body:{object_id:values.object_id}});await load();
    } else if(action==='add-goal-placement'){
      await request(`/api/sections/${state.sectionId}/goals`,{method:'POST',body:{goal_id:values.goal_id}});await load();
    } else if(action==='create-goal'||action==='edit-goal'){
      const existing=findGoal(values.goal_id);
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
      await request(`/api/goals/${values.goal_id}`,{method:'PATCH',body:{current:Number(values.current)}});await load();
    } else if(action==='add-task'){
      await request(`/api/goals/${values.goal_id}/tasks`,{method:'POST',body:{title:values.title}});await load();
    } else if(action==='change-password'){
      await request('/api/auth/password',{method:'POST',body:{current_password:values.current_password,password:values.password,confirm_password:values.confirm_password}});toast('Password updated.');
    }
    closeModal(ensureDialog(root));toast('Saved successfully.');
    } catch(error) {
      let cleanupPending=false;
      for(const galleryID of operationGalleryEntries.reverse()){
        try{await request(`/api/object-gallery/${encodeURIComponent(galleryID)}`,{method:'DELETE'});}
        catch{cleanupPending=true;}
      }
      for(const url of operationUploads){
        const assetID=url.split('/').pop();
        try {
          const result=await request(`/api/assets/${encodeURIComponent(assetID)}`,{method:'DELETE'});
          cleanupPending ||= Boolean(result?.pending_deletion);
        } catch(cleanupError) {
          // A 409 means a concurrent save already attached this image; the
          // backend correctly protects it. Other failures need user visibility.
          if(cleanupError?.code!=='image_in_use')cleanupPending=true;
        }
      }
      if(cleanupPending)error.message+=`${error.message?'\n':''}Your entered values were preserved. Unused image cleanup could not be confirmed; review Uploaded Images in account settings.`;
      throw error;
    }
  }

  async function uploadImage(file,gameID) {
    if(file.size>5*1024*1024)throw new Error('Image must be smaller than 5 MB.');const form=new FormData();form.append('file',file);if(gameID)form.append('game_id',gameID);const uploaded=await request('/api/assets',{method:'POST',body:form});return uploaded.url;
  }

  return { submitForm, uploadImage };
}
