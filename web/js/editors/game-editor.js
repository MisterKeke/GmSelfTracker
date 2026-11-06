import { descendants } from '../utils/section-tree-model.js';
import { ensureDialog } from '../modal.js';
import { sectionDisplayConfig, prettyValue, esc, renderIcon } from '../utils/formatting.js';
import { request } from '../api.js';

export function createGameEditors({ tab, openForm, fieldRow, toast, state, root }) {
  function openGameForm(edit=false, templateID='') {
    const game=edit?(state.game||{}):{};
    const templateOptions=(state.templates||[]).map(item=>`<option value="${esc(item.id)}" ${item.id===templateID?'selected':''}>${esc(item.name)} · ${Number(item.type_count)||0} types · ${Number(item.section_count)||0} sections</option>`).join('');
    const templatePicker=!edit&&templateOptions?`<div class="form-field wide"><label for="f-template_id">Start from</label><select id="f-template_id" name="template_id"><option value="">An empty game</option>${templateOptions}</select><small>Template games get fresh type, field, formula, and section IDs. No objects, goals, or private notes are copied.</small></div>`:'';
    openForm(edit?'EDIT GAME':'ADD A GAME',`${fieldRow('Game name','name',game.name||'','text',true,'required maxlength="120"')}${fieldRow('Icon or emoji','icon',game.icon||'🎮','text',true)}${templatePicker}${fieldRow('Description','description',game.description||'','text',true,'maxlength="300"')}${fieldRow('Personal notes','personal_notes',game.personal_notes||'','text',true,'maxlength="2000"')}${fieldRow('Cover image URL (optional)','cover_image',game.cover_image||'','text',true)}<div class="form-field wide"><label for="f-cover-file">Or upload a cover image</label><input id="f-cover-file" name="cover_file" type="file" accept="image/png,image/jpeg,image/gif,image/webp,image/bmp"><small>PNG, JPEG, GIF, WebP, or BMP · maximum 5 MB · animations use the first frame</small></div><input type="hidden" name="form_action" value="${edit?'edit-game':'create-game'}">`,edit?'SAVE GAME':'CREATE GAME');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=edit?'edit-game':'create-game';
    const templateSelect=form.elements.namedItem('template_id');
    const updateTemplateMode=()=>{
      const fromTemplate=Boolean(templateSelect?.value);
      for(const name of ['description','personal_notes','cover_image','cover_file']){const input=form.elements.namedItem(name);if(input){input.disabled=fromTemplate;input.closest('.form-field')?.classList.toggle('template-disabled',fromTemplate);}}
      if(templateSelect){const submit=root.querySelector('button[type="submit"][form="modal-form"]');if(submit)submit.textContent=fromTemplate?'CREATE FROM TEMPLATE':'CREATE GAME';}
    };
    templateSelect?.addEventListener('change',updateTemplateMode);updateTemplateMode();
    if(edit){form.insertAdjacentHTML('beforeend',`<div class="form-field wide" style="margin-top:10px"><button class="pixel-button danger" type="button" data-action="archive-game">Archive this game</button><small>Archived data stays saved and can be restored in settings.</small></div>`);}
  }

  function openTemplateForm(existing=null) {
    const game=state.game;
    if(!existing&&!game)throw new Error('Choose a game before saving its structure as a template.');
    const body=existing
      ?`${fieldRow('Template name','name',existing.name||'','text',true,'required maxlength="120"')}${fieldRow('Description','description',existing.description||'','text',true,'maxlength="500"')}${fieldRow('Icon or emoji','icon',existing.icon||'📘','text',true)}<input type="hidden" name="template_id" value="${esc(existing.id)}">`
      :`${fieldRow('Template name','name',`${game.name} Template`,'text',true,'required maxlength="120"')}${fieldRow('Description','description',game.description||'','text',true,'maxlength="500"')}${fieldRow('Icon or emoji','icon','📘','text',true)}<div class="form-field wide"><fieldset class="template-options"><legend>Include in the template</legend><label><input type="checkbox" name="include_sections" value="true" checked> Section structure and display settings</label><label><input type="checkbox" name="include_types" value="true" checked> Object types and field definitions</label><label><input type="checkbox" name="include_field_defaults" value="true"> Field default values</label><label><input type="checkbox" name="include_presentation" value="true" checked> Icons, primary display fields, and presentation settings</label></fieldset><p class="dialog-help">Preview: ${state.types.length} object types, ${state.types.reduce((n,type)=>n+(type.fields||[]).length,0)} fields, and ${state.sections.length} sections. Objects, goals, uploaded images, and personal notes are never included.</p></div>`;
    openForm(existing?'EDIT GAME TEMPLATE':'SAVE GAME AS TEMPLATE',`${body}<input type="hidden" name="form_action" value="${existing?'edit-template':'create-template'}">`,'SAVE TEMPLATE');
    ensureDialog(root).querySelector('#modal-form').dataset.form=existing?'edit-template':'create-template';
  }

  function openTemplateImportForm() {
    openForm('IMPORT GAME TEMPLATE',`<div class="form-field wide"><label for="f-template_file">Template JSON file</label><input id="f-template_file" name="template_file" type="file" accept="application/json,.json" required><small>Imports a versioned template structure. The file is validated before it is saved.</small></div><p class="dialog-help">Imported templates cannot contain executable code or remote resources. Personal objects and game progress are not imported.</p><input type="hidden" name="form_action" value="import-template">`,'IMPORT TEMPLATE');
    ensureDialog(root).querySelector('#modal-form').dataset.form='import-template';
  }

  function openSectionForm(existing=null) {
    const excluded=existing?descendants(state.sections,existing.id):new Set();
    const sections=state.sections.filter((item)=>!item.archived&&!excluded.has(item.id));
    const parent=existing?.parent_id||'';
    const config=sectionDisplayConfig(existing||{});
    const allFields=state.types.filter((type)=>!type.archived).flatMap((type)=>type.fields||[]).filter((field)=>!field.archived);
    const selectedFields=new Set(config.visible_field_ids||[]);
    const fieldChoices=allFields.length?allFields.map((field)=>`<label class="section-field-choice"><input type="checkbox" name="visible_field_ids" value="${esc(field.id)}" ${selectedFields.has(field.id)?'checked':''}><span>${renderIcon(field.icon||'✦')} ${esc(field.name)}</span><small>${esc(field.type_name||state.types.find((type)=>type.id===field.object_type_id)?.name||'')}</small></label>`).join(''):'<small>Create custom fields to choose what this section displays.</small>';
    const checklistFields=allFields.filter((field)=>field.field_type==='boolean');
    const progressFields=allFields.filter((field)=>field.field_type==='progress');
    const selectedOption=(fieldID,placeholder,fields)=>`<option value="">${placeholder}</option>${fields.map((field)=>`<option value="${esc(field.id)}" ${field.id===fieldID?'selected':''}>${esc(state.types.find((type)=>type.id===field.object_type_id)?.name||'')} · ${esc(field.name)}</option>`).join('')}`;
    const numericFields=allFields.filter((field)=>['integer','decimal','percentage','formula'].includes(field.field_type));
    const relationshipFields=allFields.filter((field)=>['reference','references'].includes(field.field_type));
    const fieldChecks=(name,selected)=>numericFields.map((field)=>`<label class="section-field-choice"><input type="checkbox" name="${name}" value="${esc(field.id)}" ${(selected||[]).includes(field.id)?'checked':''}><span>${esc(state.types.find((type)=>type.id===field.object_type_id)?.name||'')} · ${esc(field.name)}</span></label>`).join('');
    const progressSettings=`<div class="form-field wide"><label>Progress calculation</label><select name="progress_mode">${[['field','One current/maximum field'],['calculated','Sum numeric fields'],['children','Sum linked child progress']].map(([id,label])=>`<option value="${id}" ${(config.progress_mode||'field')===id?'selected':''}>${label}</option>`).join('')}</select><small>For calculated progress, choose fields from one type. Choose either maximum fields or a fixed maximum.</small></div><div class="form-field wide"><label>Linked progress depth</label><select name="progress_depth" ${(config.progress_mode||'field')==='children'?'':'disabled'}><option value="direct" ${(config.progress_depth||'direct')==='direct'?'selected':''}>Direct children only</option><option value="recursive" ${config.progress_depth==='recursive'?'selected':''}>Recursive leaf progress</option></select><small>Recursive mode follows same-type links, sums unique leaf progress, and ignores parent values when children exist. Cycles or missing values show a warning.</small></div><div class="form-field"><label>Calculated current: sum these fields</label>${fieldChecks('progress_current_field_ids',config.progress_current_field_ids)}</div><div class="form-field"><label>Calculated maximum: sum these fields</label>${fieldChecks('progress_max_field_ids',config.progress_max_field_ids)}</div>${fieldRow('Fixed maximum (if no maximum fields)','progress_maximum',config.progress_maximum||100,'number',false,'min="0.000001" step="any"')}<div class="form-field"><label>Children relationship field</label><select name="progress_relationship_field_id">${selectedOption(config.progress_relationship_field_id,'Choose relationship',relationshipFields)}</select></div><div class="form-field"><label>Child progress field</label><select name="progress_child_field_id">${selectedOption(config.progress_child_field_id,'Choose child progress',progressFields)}</select><small>Direct mode sums active linked objects. Recursive mode requires the relationship field to link back to the same type.</small></div>`;
    const body=`${fieldRow('Section name','name',existing?.name||'','text',true,'required maxlength="80"')}${fieldRow('Icon or emoji','icon',existing?.icon||'✦','text',false,'maxlength="2048"')}<div class="form-field"><label for="f-display_style">Display style</label><select id="f-display_style" name="display_style"><option value="cards" ${existing?.display_style==='cards'?'selected':''}>Cards</option><option value="list" ${existing?.display_style==='list'?'selected':''}>List</option><option value="table" ${existing?.display_style==='table'?'selected':''}>Table</option><option value="checklist" ${existing?.display_style==='checklist'?'selected':''}>Checklist</option><option value="progress" ${existing?.display_style==='progress'?'selected':''}>Progress</option></select></div><div class="form-field wide"><label for="f-parent_id">Parent section</label><select id="f-parent_id" name="parent_id"><option value="">Top level</option>${sections.map((item)=>`<option value="${esc(item.id)}" ${item.id===parent?'selected':''}>${esc(item.name)}</option>`).join('')}</select><small>Sections can be nested as deeply as you like.</small></div><div class="form-field wide"><label>Visible fields for cards and tables</label><div class="section-field-choices">${fieldChoices}</div><small>Leave all unchecked to use the default fields in each view.</small></div><div class="form-field"><label for="f-checklist_field_id">Checklist checkbox field</label><select name="checklist_field_id" id="f-checklist_field_id">${selectedOption(config.checklist_field_id,'Choose a checkbox field',checklistFields)}</select></div><div class="form-field"><label for="f-progress_field_id">Progress field</label><select name="progress_field_id" id="f-progress_field_id">${selectedOption(config.progress_field_id,'Choose a progress field',progressFields)}</select></div>${progressSettings}${existing?fieldRow('Sort position','sort_order',existing.sort_order,'number',false,'step="1"'):''}<input type="hidden" name="form_action" value="${existing?'edit-section':'create-section'}">${existing?`<input type="hidden" name="section_id" value="${esc(existing.id)}">`:''}`;
    openForm(existing?'EDIT SECTION':'CREATE A SECTION',body,existing?'SAVE SECTION':'CREATE SECTION');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=existing?'edit-section':'create-section';
    form.elements.progress_mode.addEventListener('change',()=>{form.elements.progress_depth.disabled=form.elements.progress_mode.value!=='children';});
  }

  function openTypeForm(existing=null) {
    const fields=(existing?.fields||[]).filter(field=>!field.archived||field.id===existing?.primary_display_field_id);
    const primary=existing?`<label class="form-field wide">Primary display field<select name="primary_display_field_id" data-primary-field><option value="">None</option>${fields.map(field=>`<option value="${esc(field.id)}" ${field.id===existing.primary_display_field_id?'selected':''}>${esc(field.name)}${field.archived?' (archived; hidden)':''}</option>`).join('')}</select><small>Emphasize one of this type's fields on object cards. Archived fields use the normal card fallback.</small></label><div class="primary-preview wide" aria-live="polite"></div>`:'<p class="dialog-help wide">After creating fields, edit this type to choose a primary display field.</p>';
    openForm(existing?'EDIT OBJECT TYPE':'CREATE AN OBJECT TYPE',`${fieldRow('Type name','name',existing?.name||'','text',true,'required maxlength="80"')}${fieldRow('Icon or emoji','icon',existing?.icon||'◈','text')}${fieldRow('Description','description',existing?.description||'','text',true,'maxlength="300"')}<div class="form-field wide"><label>Type image</label><input name="type_image_file" type="file" accept="image/png,image/jpeg,image/gif,image/webp,image/bmp"><small>Maximum 5 MB. Used as the default image for this type's objects.</small></div>${primary}${existing?`<input type="hidden" name="object_type_id" value="${esc(existing.id)}">`:''}`,existing?'SAVE TYPE':'CREATE TYPE');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=existing?'edit-type':'create-type';
    if(existing){
      const preview=()=>{
        const field=fields.find(item=>item.id===form.elements.primary_display_field_id.value&&!item.archived);
        const object=state.objects.find(item=>item.object_type_id===existing.id);
        form.querySelector('.primary-preview').textContent=`${object?.name||'Object preview'}${field?` · ${field.name}: ${object?.formula_errors?.[field.id]||prettyValue(object?.values?.[field.id],field,state.objects)}`:' · No primary field displayed'}`;
      };
      form.elements.primary_display_field_id.addEventListener('change',preview);preview();
    }
  }

  function openAddExisting() {
    const sectionID=state.sectionId;
    openForm('ADD EXISTING OBJECT',`<p class="dialog-help">Choose an object to place here. Its values remain shared with every section.</p><div class="form-field wide"><label for="placement-search">Search objects</label><input id="placement-search" type="search" data-placement-search placeholder="Name, type or description"><small data-placement-status role="status">Loading eligible objects…</small></div><div class="form-field wide"><label for="f-object_id">Object</label><select id="f-object_id" name="object_id" required><option value="">Choose an object</option></select></div>`, 'ADD TO SECTION');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form='add-placement';
    const input=form.querySelector('[data-placement-search]');let version=0,timer;
    const search=async()=>{
      const current=++version,params=new URLSearchParams({limit:'30',offset:'0',exclude_section_id:sectionID,search:input.value||''});
      try{
        const result=await request(`/api/games/${encodeURIComponent(state.game.id)}/objects?${params}`);
        if(!input.isConnected||current!==version)return;
        const select=form.elements.object_id,selected=select.value;
        select.innerHTML=`<option value="">Choose an object</option>${(result.items||[]).map(item=>`<option value="${esc(item.id)}">${esc(item.name)} · ${esc(item.type_name)}</option>`).join('')}`;
        select.value=selected;
        form.querySelector('[data-placement-status]').textContent=`${result.total} eligible object${result.total===1?'':'s'} · Search finds objects on later pages too.`;
      }catch(error){if(input.isConnected&&current===version)form.querySelector('[data-placement-status]').textContent=error.message;}
    };
    input.addEventListener('input',()=>{clearTimeout(timer);timer=setTimeout(search,250);});search();
  }

  function openAddExistingGoal() {
    openForm('ADD GOAL TO SECTION',`<p class="dialog-help">Goals remain shared across sections. Search the game's goals and add a placement here.</p><div class="form-field wide"><label>Search goals<input type="search" data-placement-goal-search placeholder="Find a goal…"><small data-placement-goal-status role="status">Loading available goals…</small></label></div><div class="form-field wide"><label>Goal<select name="goal_id" required><option value="">Choose a goal</option></select></label></div>`,'ADD GOAL');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form='add-goal-placement';
    const input=form.querySelector('[data-placement-goal-search]');let version=0,timer;
    const search=async()=>{
      const current=++version,params=new URLSearchParams({exclude_section_id:state.sectionId,search:input.value||'',sort:'title',direction:'asc',limit:'30',offset:'0'});
      const status=form.querySelector('[data-placement-goal-status]'),select=form.elements.goal_id;status.textContent='Searching available goals…';
      try{
        const result=await request(`/api/games/${encodeURIComponent(state.game.id)}/goals?${params}`);
        if(!input.isConnected||version!==current)return;
        const selected=select.value;select.innerHTML=`<option value="">Choose a goal</option>${(result.items||[]).map(goal=>{state.goalCache?.set(goal.id,goal);return `<option value="${esc(goal.id)}">${esc(goal.title)}</option>`;}).join('')}`;select.value=selected;
        status.textContent=`${result.total} available goal${result.total===1?'':'s'}; search finds goals beyond the first page.`;
      }catch(error){if(input.isConnected&&version===current)status.textContent=error.message;}
    };
    input.addEventListener('input',()=>{clearTimeout(timer);timer=setTimeout(search,250);});search();
  }

  return { openGameForm, openTemplateForm, openTemplateImportForm, openSectionForm, openTypeForm, openAddExisting, openAddExistingGoal };
}
