import { renderFormulaEditor, bindFormulaEditor } from './formula-editor.js';
import { ensureDialog } from '../modal.js';
import { FIELD_TYPES, esc, formatDefaultValue, readDefaultValue } from '../utils/formatting.js';
import { request } from '../api.js';

export function createFieldEditor({ openForm, fieldRow, state, root }) {
  let migrationReferenceOptions=[];
  let migrationReferenceSearchSequence=0;
  function openFieldForm(typeID,existing=null) {
    const type=state.types.find((item)=>item.id===(typeID||existing?.object_type_id));if(!type)return;
    const selectedType=existing?.field_type||'text';
    const options=existing?.options||{};
    const defaultValue=formatDefaultValue(existing?.default_value);
    let body=`${fieldRow('Field name','name',existing?.name||'','text',true,'required maxlength="80"')}${fieldRow('Icon or emoji','icon',existing?.icon||'✦','text')}`;
    body+=`<div class="form-field wide"><label for="f-field_type">Field type ${existing?'(change through the migration wizard)':''}</label><select id="f-field_type" name="field_type" ${existing?'disabled':''}>${FIELD_TYPES.map(([id,label])=>`<option value="${id}" ${selectedType===id?'selected':''}>${label}</option>`).join('')}</select></div>`;
    body+=fieldRow('Description','description',existing?.description||'','text',true,'maxlength="300"');
    body+=`<div class="form-field wide" id="field-kind-options"></div>`;
    body+=`<div class="form-field wide" id="default-field">${fieldRow('Default value (optional)','default_value',defaultValue,'text',true,'maxlength="1000"')}</div>`;
    if(existing)body+=`<input type="hidden" name="field_type" value="${esc(existing.field_type)}"><input type="hidden" name="field_id" value="${esc(existing.id)}"><input type="hidden" name="object_type_id" value="${esc(existing.object_type_id)}">`;
    else body+=`<input type="hidden" name="object_type_id" value="${esc(type.id)}">`;
    body+=`<p class="dialog-help wide">Change an existing field’s type with the migration wizard so saved values can be previewed and validated first.</p>`;
    openForm(existing?'EDIT FIELD':'ADD A FIELD',body,existing?'SAVE FIELD':'CREATE FIELD');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=existing?'edit-field':'create-field';
    const typeSelect=form.querySelector('#f-field_type');
    const renderOptions=()=>{form.querySelector('#field-kind-options').innerHTML=fieldOptionsHTML(typeSelect.value,options,type.fields||[],Boolean(existing),existing?.id||'');form.querySelector('#default-field').hidden=['formula','reference','references'].includes(typeSelect.value);if(typeSelect.value==='formula')bindFormulaEditor(form,type,state,existing?.id||'');};
    typeSelect.addEventListener('change',()=>{renderOptions();renderChoiceMigrations(form,options.choices||[]);});renderOptions();
    if(existing){form.addEventListener('input',(event)=>{if(event.target.name==='choices')renderChoiceMigrations(form,options.choices||[]);});renderChoiceMigrations(form,options.choices||[]);}
  }

  function openFieldTypeMigration(existing) {
    const type=state.types.find((item)=>item.id===existing?.object_type_id);if(!type)return;
    migrationReferenceOptions=[];migrationReferenceSearchSequence++;
    const targetTypes=FIELD_TYPES.filter(([id])=>id!=='formula');
    const preferred=existing.field_type==='formula'?['decimal','integer','percentage']:[];
    const initialTarget=preferred.find(id=>targetTypes.some(([candidate])=>candidate===id))||targetTypes.find(([id])=>id!==existing.field_type)?.[0]||'';
    const body=`<section class="light-panel migration-current"><strong>${esc(existing.name)}</strong><p>${esc(existing.field_type)} → choose a target type</p><small>Objects, including archived objects, are included in the preview.</small></section><div class="form-field wide"><label for="migration-target-type">New field type</label><select id="migration-target-type" name="target_type">${targetTypes.map(([id,label])=>`<option value="${id}" ${id===existing.field_type?'disabled':''} ${id===initialTarget?'selected':''}>${esc(label)}</option>`).join('')}</select></div><div class="form-field wide" data-migration-options></div><div class="form-field wide" data-migration-policy></div><div class="form-field wide"><label for="migration-default-action">Default value</label><select id="migration-default-action" name="default_action"><option value="convert">Convert the current default automatically</option><option value="clear">Clear the default</option><option value="set">Set a new target default</option></select><input id="migration-default-value" name="migration_default" type="text" maxlength="50000" placeholder="New default value" hidden></div><section class="migration-preview" data-migration-preview aria-live="polite"><p>Choose a target type, then preview the conversion.</p></section><input type="hidden" name="field_id" value="${esc(existing.id)}"><input type="hidden" name="object_type_id" value="${esc(type.id)}">`;
    openForm('CHANGE FIELD TYPE',body,'PREVIEW CONVERSION');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form='field-migration-preview';
    const targetSelect=form.elements.namedItem('target_type');
    const renderControls=()=>{
      const target=targetSelect.value;
      const options=form.querySelector('[data-migration-options]');
      if(target==='select')options.innerHTML=`<label for="migration-choices">Target choices (one per line)</label><textarea id="migration-choices" name="choices" required></textarea><small>Existing text values must match one of these choices or have an explicit replacement below.</small>`;
      else if(target==='reference'||target==='references'){
        const currentAllowed=['reference','references'].includes(existing.field_type)?existing.options?.allowed_type_id||'':'';
        options.innerHTML=`<label for="migration-allowed-type">Allowed target type</label><select id="migration-allowed-type" name="allowed_type_id" data-migration-allowed-type><option value="">Any type in this game</option>${state.types.filter((item)=>!item.archived).map((item)=>`<option value="${esc(item.id)}" ${currentAllowed===item.id?'selected':''}>${esc(item.name)}</option>`).join('')}</select><label for="migration-reference-search">Search target objects</label><input id="migration-reference-search" type="search" data-migration-reference-search placeholder="Search object names"><small data-migration-reference-status role="status">Search to populate object choices for explicit text mappings.</small>`;
        searchMigrationReferences(form);
      }
      else options.innerHTML=`<small>${target==='formula'?'Formula target migration is not supported by this wizard.':''}</small>`;
      const policy=form.querySelector('[data-migration-policy]');
      const controls=[];
      if(target==='progress'&&['integer','decimal','percentage'].includes(existing.field_type))controls.push(`<label for="migration-maximum">Progress maximum</label><input id="migration-maximum" name="maximum" type="number" step="any" min="0.0000000001" required placeholder="For example, 100">`);
      if(existing.field_type==='progress'&&['integer','decimal','percentage'].includes(target))controls.push(`<label for="migration-progress-value">Use this value from each progress item</label><select id="migration-progress-value" name="progress_value"><option value="">Choose a strategy</option><option value="current">Current value</option><option value="maximum">Maximum value</option><option value="percentage">Current ÷ maximum × 100</option></select>`);
      if(['reference','references'].includes(existing.field_type)&&['text','long_text'].includes(target))controls.push(`<label for="migration-relationship-strategy">Preserve existing links</label><select id="migration-relationship-strategy" name="relationship_strategy"><option value="">Choose a strategy</option><option value="serialize_ids">Serialize linked object IDs as JSON text</option></select><small>Use Long text when a field can contain many linked IDs.</small>`);
      if(target==='percentage')controls.push(`<label><input type="checkbox" name="percentage_clamp"> Clamp out-of-range values to 0 or 100</label><small>Clamping changes the stored value. Without this choice, values outside the percentage range block the migration.</small>`);
      if(existing.field_type==='formula'&&['integer','decimal','percentage'].includes(target))controls.push(`<label><input type="checkbox" name="freeze_formula"> Freeze each current formula result into a stored value</label><small>Later changes to formula inputs will no longer update these values.</small>`);
      policy.innerHTML=controls.length?controls.join(''):'<small>No additional conversion policy is needed for this pair.</small>';
      const defaultInput=form.querySelector('[name=migration_default]');
      defaultInput.hidden=form.elements.namedItem('default_action').value!=='set'||['formula','reference','references'].includes(target);
      form.querySelector('[data-migration-preview]').innerHTML='<p>Preview the conversion to see affected objects, dependencies, and incompatible values.</p>';
    };
    targetSelect.addEventListener('change',renderControls);
    form.elements.namedItem('default_action').addEventListener('change',()=>{const input=form.elements.namedItem('migration_default');input.hidden=form.elements.namedItem('default_action').value!=='set';});
    form.addEventListener('input',(event)=>{
      if(event.target.matches('[data-migration-reference-search]')){
        const sequence=++migrationReferenceSearchSequence;
        setTimeout(()=>{if(sequence===migrationReferenceSearchSequence)searchMigrationReferences(form);},200);
      }
    });
    form.addEventListener('change',(event)=>{if(event.target.matches('[data-migration-allowed-type]'))searchMigrationReferences(form);});
    const invalidatePreview=(event)=>{
      if(event.target.name==='target_type')return;
      if(event.target.closest('[data-migration-preview]')&&!event.target.matches('[data-migration-replacement]'))return;
      form.dataset.previewToken='';form.dataset.form='field-migration-preview';
      const submit=ensureDialog(root).querySelector('button[form="modal-form"][type="submit"]');if(submit)submit.textContent='PREVIEW CONVERSION';
      const output=form.querySelector('[data-migration-preview]');if(output)output.dataset.stale='true';
    };
    form.addEventListener('input',invalidatePreview);
    form.addEventListener('change',invalidatePreview);
    targetSelect.addEventListener('change',()=>{form.dataset.previewToken='';form.dataset.form='field-migration-preview';const submit=ensureDialog(root).querySelector('button[form="modal-form"][type="submit"]');if(submit)submit.textContent='PREVIEW CONVERSION';});
    renderControls();
  }

  async function searchMigrationReferences(form) {
    const search=form.querySelector('[data-migration-reference-search]');if(!search)return;
    const sequence=++migrationReferenceSearchSequence;
    const status=form.querySelector('[data-migration-reference-status]');
    if(status)status.textContent='Searching objects…';
    const params=new URLSearchParams({limit:'20',offset:'0',search:search.value.trim()});
    const typeID=form.elements.namedItem('allowed_type_id')?.value;if(typeID)params.set('type_id',typeID);
    try {
      const result=await request(`/api/games/${encodeURIComponent(state.game.id)}/objects?${params}`);
      if(sequence!==migrationReferenceSearchSequence||!form.isConnected)return;
      migrationReferenceOptions=result.items||[];
      for(const select of form.querySelectorAll('[data-migration-reference-replacement]')){
        const selected=select.value,selectedLabel=select.selectedOptions[0]?.textContent||'';
        select.innerHTML=`<option value="">Choose an object</option>${selected&&!migrationReferenceOptions.some(item=>item.id===selected)?`<option value="${esc(selected)}" selected>${esc(selectedLabel)}</option>`:''}${migrationReferenceOptions.map(item=>`<option value="${esc(item.id)}" ${selected===item.id?'selected':''}>${esc(item.name)} · ${esc(item.type_name)}</option>`).join('')}`;
      }
      if(status)status.textContent=migrationReferenceOptions.length?`${migrationReferenceOptions.length} matching target objects. Search to narrow the list.`:'No matching active objects found.';
    } catch(error) {
      if(sequence===migrationReferenceSearchSequence&&status)status.textContent=`Could not search target objects: ${error.message}`;
    }
  }

  function migrationRequest(form,existing) {
    const target=form.elements.namedItem('target_type').value;
    const options={};
    if(target==='select')options.choices=(form.elements.namedItem('choices')?.value||'').split('\n').map((item)=>item.trim()).filter(Boolean);
    if((target==='reference'||target==='references')&&form.elements.namedItem('allowed_type_id')?.value)options.allowed_type_id=form.elements.namedItem('allowed_type_id').value;
    const policy={};
    const maximum=form.elements.namedItem('maximum')?.value;if(maximum)policy.maximum=Number(maximum);
    const progress=form.elements.namedItem('progress_value')?.value;if(progress)policy.progress_value=progress;
    const relationshipStrategy=form.elements.namedItem('relationship_strategy')?.value;if(relationshipStrategy)policy.relationship_strategy=relationshipStrategy;
    if(form.elements.namedItem('percentage_clamp')?.checked)policy.percentage_strategy='clamp';
    if(form.elements.namedItem('freeze_formula')?.checked)policy.freeze_formula=true;
    const replacements={};for(const select of form.querySelectorAll('[data-migration-replacement]'))if(select.value)replacements[select.dataset.migrationReplacement]=select.value;
    if(Object.keys(replacements).length)policy.select_replacements=replacements;
    const referenceMappings={};for(const select of form.querySelectorAll('[data-migration-reference-replacement]'))if(select.value)referenceMappings[select.dataset.migrationReferenceReplacement]=select.value;
    if(Object.keys(referenceMappings).length)policy.reference_mapping=referenceMappings;
    const payload={target_type:target,options,policy};
    const defaultAction=form.elements.namedItem('default_action').value;
    if(defaultAction==='clear')payload.default_value=null;
    if(defaultAction==='set'&&!['formula','reference','references'].includes(target))payload.default_value=readDefaultValue(form.elements.namedItem('migration_default').value,target);
    const previewToken=form.dataset.previewToken;if(previewToken)payload.preview_token=previewToken;
    return {fieldID:existing.id,payload};
  }

  function renderMigrationPreview(form,summary) {
    const output=form.querySelector('[data-migration-preview]');
    const previousReplacements=Object.fromEntries([...form.querySelectorAll('[data-migration-replacement],[data-migration-reference-replacement]')].map(select=>[select.dataset.migrationReplacement||select.dataset.migrationReferenceReplacement,select.value]));
    const counts=`<div class="migration-counts"><span><strong>${Number(summary.affected_objects)||0}</strong> affected objects</span><span><strong>${Number(summary.saved_values)||0}</strong> saved values</span><span><strong>${Number(summary.archived_objects_affected)||0}</strong> archived objects</span><span><strong>${Number(summary.preset_overrides)||0}</strong> preset overrides</span><span><strong>${Number(summary.incompatible_values)||0}</strong> incompatible values</span></div>`;
    const dependencies=(summary.dependencies||[]).length?`<h3>Dependencies</h3><ul>${summary.dependencies.map(item=>`<li>${esc(item)}</li>`).join('')}</ul>`:'<p>No field dependencies were found.</p>';
    const blockers=(summary.blockers||[]).length?`<h3>Needs attention</h3><ul class="migration-blockers">${summary.blockers.map(item=>`<li>${esc(item)}</li>`).join('')}</ul>`:'';
    const examples=(summary.incompatible_examples||[]).map(item=>{
      const value=typeof item.value==='string'?item.value:JSON.stringify(item.value??'');
      const mapping=summary.target_type==='select'&&form.elements.namedItem('choices')?`<label>Replace “${esc(value)}” with<select data-migration-replacement="${esc(value)}"><option value="">Choose a target choice</option>${form.elements.namedItem('choices').value.split('\n').map(choice=>choice.trim()).filter(Boolean).map(choice=>`<option value="${esc(choice)}" ${previousReplacements[value]===choice?'selected':''}>${esc(choice)}</option>`).join('')}</select></label>`:(['reference','references'].includes(summary.target_type)&&['text','long_text','select'].includes(existing.field_type)?`<label>Map “${esc(value)}” to<select data-migration-reference-replacement="${esc(value)}"><option value="">Choose an object</option>${migrationReferenceOptions.map(item=>`<option value="${esc(item.id)}" ${previousReplacements[value]===item.id?'selected':''}>${esc(item.name)} · ${esc(item.type_name)}</option>`).join('')}</select></label>`:'');
      return `<li><strong>${esc(item.object_name)}</strong><code>${esc(value)}</code><p>${esc(item.reason)}</p>${mapping}</li>`;
    }).join('');
    const defaultSummary=`<p><strong>Default:</strong> ${summary.current_default===undefined?'none':esc(JSON.stringify(summary.current_default))} → ${summary.converted_default===undefined?'none':esc(JSON.stringify(summary.converted_default))}${summary.default_error?` · ${esc(summary.default_error)}`:''}</p>`;
    output.innerHTML=`<h3>Migration preview</h3>${counts}${defaultSummary}${summary.formula_warning?`<p class="dialog-help">${esc(summary.formula_warning)}</p>`:''}${dependencies}${blockers}${examples?`<h3>Incompatible examples</h3><ul class="migration-examples">${examples}</ul>`:''}${summary.can_apply?'<p class="migration-safe" role="status">All reviewed values can be converted safely.</p>':'<p class="form-error">Resolve the listed issues, then preview again.</p>'}`;
    output.dataset.stale='false';
    form.dataset.previewed='true';
    form.dataset.previewToken=summary.preview_token||'';
    form.dataset.form=summary.can_apply?'field-migration-apply':'field-migration-preview';
    const submit=ensureDialog(root).querySelector('button[form="modal-form"][type="submit"]');if(submit)submit.textContent=summary.can_apply?'APPLY CONVERSION':'PREVIEW AGAIN';
  }

  function renderChoiceMigrations(form,oldChoices){
    const container=form.querySelector('#choice-migrations');if(!container)return;
    const previous=new Map([...container.querySelectorAll('[data-replace-choice]')].map((select)=>[select.dataset.replaceChoice,select.value]));
    const choices=(form.elements.namedItem('choices')?.value||'').split('\n').map((item)=>item.trim()).filter(Boolean);
    container.innerHTML=oldChoices.filter((choice)=>!choices.includes(choice)).map((choice)=>`<label>Replace removed choice “${esc(choice)}”<select data-replace-choice="${esc(choice)}"><option value="">No replacement (only allowed if unused)</option>${choices.map((replacement)=>`<option value="${esc(replacement)}" ${previous.get(choice)===replacement?'selected':''}>${esc(replacement)}</option>`).join('')}</select></label>`).join('');
  }

  function fieldOptionsHTML(fieldType,options={},typeFields=[],editing=false,editingID='') {
    if(fieldType==='select')return `<label for="f-choices">Choices (one per line)</label><textarea id="f-choices" name="choices" required>${esc((options.choices||[]).join('\n'))}</textarea>${editing?`<div id="choice-migrations"></div><small>Removed choices used by objects, defaults, or goal rules require a replacement.</small>`:''}`;
    if(fieldType==='reference'||fieldType==='references')return `<label for="f-allowed_type_id">Allowed target type</label><select id="f-allowed_type_id" name="allowed_type_id"><option value="">Any type in this game</option>${state.types.filter((item)=>!item.archived||item.id===options.allowed_type_id).map((item)=>`<option value="${esc(item.id)}" ${options.allowed_type_id===item.id?'selected':''}>${esc(item.name)}${item.archived?' (archived)':''}</option>`).join('')}</select>${editing?'<label><input type="checkbox" name="remove_incompatible_references"> Remove existing references that do not match the new allowed type</label><small>This includes references on archived objects. Leaving this unchecked preserves data and blocks incompatible changes.</small>':''}`;
    if(fieldType==='formula')return renderFormulaEditor(options,typeFields,state,editingID);
    if(fieldType==='progress')return `<small>Enter the current and maximum values on each object. A percentage is calculated for display.</small>`;
    return '';
  }

  return { openFieldForm, openFieldTypeMigration, migrationRequest, renderMigrationPreview, renderChoiceMigrations, fieldOptionsHTML };
}
