import { ensureDialog } from '../modal.js';
import { esc } from '../utils/formatting.js';

export function createObjectEditor({ openForm, fieldRow, openTypeForm, toast, state, root }) {
  function openObjectForm(object=null) {
    const type=object?state.types.find((item)=>item.id===object.object_type_id):state.types.find((item)=>item.id===root.querySelector('[data-selected-type]')?.dataset.selectedType)||state.types.find((item)=>!item.archived);
    if(!type){openTypeForm();toast('Create an object type first, then add objects.',true);return;}
    const activeFields=(type.fields||[]).filter((field)=>!field.archived);
    const imageInput=`<div class="form-field wide"><label for="f-object-file">Object image (optional)</label><input id="f-object-file" name="image_file" type="file" accept="image/png,image/jpeg,image/gif,image/webp,image/bmp"><small>PNG, JPEG, GIF, WebP, or BMP · maximum 5 MB · animations use the first frame</small></div>`;
    const fields=activeFields.map((field)=>renderFieldInput(field,object?.values?.[field.id],object)).join('');
    const typeSelect=object?'':`<div class="form-field wide"><label for="f-object_type_id">Object type</label><select id="f-object_type_id" name="object_type_id" data-action="choose-object-type">${state.types.filter((item)=>!item.archived).map((item)=>`<option value="${esc(item.id)}" ${item.id===type.id?'selected':''}>${esc(item.name)}</option>`).join('')}</select></div><div class="form-field wide" id="object-field-inputs">${fields}</div>`;
    const simple=`${fieldRow('Object name','name',object?.name||'','text',true,'required maxlength="160"')}${fieldRow('Description','description',object?.description||'','text',true,'maxlength="500"')}`;
    const sectionSelector=!object&&state.panel==='section'?`<div class="form-field wide"><label for="f-section_id">Also place in section</label><select name="section_id" id="f-section_id"><option value="">Do not place in a section</option>${state.sections.filter((item)=>!item.archived).map((item)=>`<option value="${esc(item.id)}" ${item.id===state.sectionId?'selected':''}>${esc(item.name)}</option>`).join('')}</select></div>`:'';
    const formAction=object?'edit-object':'create-object';
    openForm(object?'EDIT OBJECT':'CREATE AN OBJECT',`${simple}${typeSelect}${object?`<div class="form-field wide">Type: <strong>${esc(type.name)}</strong></div>`:''}<div class="form-field wide" id="object-fields">${object?fields:''}</div>${imageInput}${sectionSelector}<input type="hidden" name="type_id" value="${esc(type.id)}">${object?`<input type="hidden" name="object_id" value="${esc(object.id)}">`:''}<p class="dialog-help wide">Objects belong to the game. You can place the same object in several sections.</p>`,object?'SAVE OBJECT':'CREATE OBJECT');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=formAction;form.dataset.typeId=type.id;
  }

  function renderFieldInput(field,rawValue,object) {
    const value=rawValue===undefined&&!object?field.default_value:rawValue;
    const name=`${field.id}`;
    const label=field.name;
    if(field.field_type==='formula')return `<div class="form-field wide"><label>${esc(label)} · calculated</label><input readonly value="${esc(object?.formula_errors?.[field.id]||(value??'Calculated when inputs are filled'))}"><small>${esc(object?.formula_errors?.[field.id]||'This value comes from your formula configuration.')}</small></div>`;
    if(field.field_type==='boolean')return `<div class="form-field"><label for="f-${esc(name)}">${esc(label)}</label><input id="f-${esc(name)}" name="${esc(name)}" type="checkbox" ${value?'checked':''}></div>`;
    if(field.field_type==='long_text')return `<div class="form-field wide"><label for="f-${esc(name)}">${esc(label)}</label><textarea id="f-${esc(name)}" name="${esc(name)}" maxlength="50000">${esc(value??'')}</textarea></div>`;
    if(field.field_type==='select'){const options=field.options||{};return `<div class="form-field"><label for="f-${esc(name)}">${esc(label)}</label><select id="f-${esc(name)}" name="${esc(name)}"><option value="">Choose…</option>${(options.choices||[]).map((choice)=>`<option value="${esc(choice)}" ${choice===value?'selected':''}>${esc(choice)}</option>`).join('')}</select></div>`;}
    if(field.field_type==='reference'||field.field_type==='references'){const options=field.options||{};const targets=state.objects.filter((item)=>item.id!==object?.id&&(!options.allowed_type_id||item.object_type_id===options.allowed_type_id));const selected=new Set(Array.isArray(value)?value:[value].filter(Boolean));const missing=[...selected].filter((id)=>!targets.some((item)=>item.id===id));return `<div class="form-field ${field.field_type==='references'?'wide':''}"><label for="f-${esc(name)}">${esc(label)}</label><input type="search" data-reference-search="${esc(name)}" aria-label="Search ${esc(label)} references" placeholder="Find an object…"><select id="f-${esc(name)}" name="${esc(name)}" ${field.field_type==='references'?'multiple':''} ${field.field_type==='references'?'size="4"':''}><option value="">${field.field_type==='references'?'Select one or more…':'No reference'}</option>${missing.map((id)=>`<option value="${esc(id)}" selected>${esc(id)} · unavailable (preserved)</option>`).join('')}${targets.map((target)=>`<option value="${esc(target.id)}" ${selected.has(target.id)?'selected':''}>${esc(target.name)} · ${esc(target.type_name)}</option>`).join('')}</select></div>`;}
    if(field.field_type==='progress'){const current=value?.current??'';const max=value?.max??'';return `<div class="form-field"><label>${esc(label)}</label><div style="display:flex;gap:6px"><input aria-label="${esc(label)} current" type="number" step="any" name="${esc(name)}__current" data-progress-field="${esc(name)}" value="${esc(current)}" placeholder="Current"><input aria-label="${esc(label)} maximum" type="number" step="any" name="${esc(name)}__max" data-progress-field="${esc(name)}" value="${esc(max)}" placeholder="Maximum"></div></div>`;}
    const htmlType=field.field_type==='date'?'date':field.field_type==='integer'?'number':field.field_type==='decimal'||field.field_type==='percentage'?'number':'text';
    const step=field.field_type==='integer'?'1':field.field_type==='decimal'||field.field_type==='percentage'?'any':null;
    const inputType=htmlType==='number'?`type="number" step="${step}"`: `type="${htmlType}"`;
    return `<div class="form-field"><label for="f-${esc(name)}">${esc(label)}</label><input id="f-${esc(name)}" name="${esc(name)}" ${inputType} value="${esc(value??'')}" ${field.field_type==='percentage'?'min="0" max="100"':''}></div>`;
  }

  function readValues(form, type) {
    const values={};
    for(const field of (type.fields||[]).filter((item)=>!item.archived)){
      if(field.field_type==='formula')continue;
      const el=form.elements.namedItem(field.id);
      if(field.field_type==='boolean'){values[field.id]=Boolean(el?.checked);continue;}
      if(field.field_type==='reference'||field.field_type==='references'){
        const selected=[...(el?.selectedOptions||[])].map((option)=>option.value).filter(Boolean);
        values[field.id]=field.field_type==='reference'?(selected[0]||null):selected;continue;
      }
      if(field.field_type==='progress'){
        const current=form.elements.namedItem(`${field.id}__current`)?.value;
        const max=form.elements.namedItem(`${field.id}__max`)?.value;
        values[field.id]=current===''&&max===''?null:{current:Number(current||0),max:Number(max||0)};continue;
      }
      let value=el?.value??'';
      if(value===''){values[field.id]=null;continue;}
      if(['integer','decimal','percentage'].includes(field.field_type))value=Number(value);
      values[field.id]=value;
    }
    return values;
  }

  root.addEventListener('input',event=>{
    if(!event.target.dataset.referenceSearch)return;
    const select=event.target.closest('.form-field').querySelector('select');
    const query=event.target.value.toLocaleLowerCase();
    for(const option of select.options)option.hidden=Boolean(option.value&&!option.selected&&!option.textContent.toLocaleLowerCase().includes(query));
  });

  return { openObjectForm, renderFieldInput, readValues };
}
