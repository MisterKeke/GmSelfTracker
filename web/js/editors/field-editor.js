import { renderFormulaEditor, bindFormulaEditor } from './formula-editor.js';
import { ensureDialog } from '../modal.js';
import { FIELD_TYPES, esc, formatDefaultValue } from '../utils/formatting.js';

export function createFieldEditor({ openForm, fieldRow, state, root }) {
  function openFieldForm(typeID,existing=null) {
    const type=state.types.find((item)=>item.id===(typeID||existing?.object_type_id));if(!type)return;
    const selectedType=existing?.field_type||'text';
    const options=existing?.options||{};
    const defaultValue=formatDefaultValue(existing?.default_value);
    let body=`${fieldRow('Field name','name',existing?.name||'','text',true,'required maxlength="80"')}${fieldRow('Icon or emoji','icon',existing?.icon||'✦','text')}`;
    body+=`<div class="form-field wide"><label for="f-field_type">Field type ${existing?'(locked after creation)':''}</label><select id="f-field_type" name="field_type" ${existing?'disabled':''}>${FIELD_TYPES.map(([id,label])=>`<option value="${id}" ${selectedType===id?'selected':''}>${label}</option>`).join('')}</select></div>`;
    body+=fieldRow('Description','description',existing?.description||'','text',true,'maxlength="300"');
    body+=`<div class="form-field wide" id="field-kind-options"></div>`;
    body+=`<div class="form-field wide" id="default-field">${fieldRow('Default value (optional)','default_value',defaultValue,'text',true,'maxlength="1000"')}</div>`;
    if(existing)body+=`<input type="hidden" name="field_type" value="${esc(existing.field_type)}"><input type="hidden" name="field_id" value="${esc(existing.id)}"><input type="hidden" name="object_type_id" value="${esc(existing.object_type_id)}">`;
    else body+=`<input type="hidden" name="object_type_id" value="${esc(type.id)}">`;
    body+=`<p class="dialog-help wide">The field type is permanent once created. You can rename, reorder, and archive the field later; archived values remain saved.</p>`;
    openForm(existing?'EDIT FIELD':'ADD A FIELD',body,existing?'SAVE FIELD':'CREATE FIELD');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=existing?'edit-field':'create-field';
    const typeSelect=form.querySelector('#f-field_type');
    const renderOptions=()=>{form.querySelector('#field-kind-options').innerHTML=fieldOptionsHTML(typeSelect.value,options,type.fields||[],Boolean(existing),existing?.id||'');form.querySelector('#default-field').hidden=['formula','reference','references'].includes(typeSelect.value);if(typeSelect.value==='formula')bindFormulaEditor(form,type,state,existing?.id||'');};
    typeSelect.addEventListener('change',()=>{renderOptions();renderChoiceMigrations(form,options.choices||[]);});renderOptions();
    if(existing){form.addEventListener('input',(event)=>{if(event.target.name==='choices')renderChoiceMigrations(form,options.choices||[]);});renderChoiceMigrations(form,options.choices||[]);}
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

  return { openFieldForm, renderChoiceMigrations, fieldOptionsHTML };
}
