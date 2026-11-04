import { request } from '../api.js';
import { esc, renderIcon, typeLabel } from '../utils/formatting.js';

const numericKinds=['integer','decimal','percentage','progress','formula'];
export function renderFormulaEditor(options,typeFields,state,editingID='') {
  const aggregation=options.aggregation||{};
  const fields=typeFields.filter(field=>!field.archived&&numericKinds.includes(field.field_type)&&field.id!==editingID);
  const relationships=typeFields.filter(field=>!field.archived&&['reference','references'].includes(field.field_type));
  return `<div class="formula-builder"><strong>Available fields</strong><div class="formula-field-picker">${fields.map(field=>`<button type="button" class="pixel-button small soft" data-formula-insert="@{${esc(field.id)}}">${renderIcon(field.icon)} ${esc(field.name)} <small>${esc(typeLabel(field.field_type))}</small></button>`).join('')||'<small>Create a numeric input field first, or configure relationship aggregation below.</small>'}</div><div class="formula-operators" role="group" aria-label="Formula operators">${['+','-','*','/','(',')'].map(op=>`<button type="button" class="pixel-button small" data-formula-insert="${op}" aria-label="Insert ${op}">${op}</button>`).join('')}<button type="button" class="pixel-button small" data-formula-insert="@{related}">Relationship result</button></div><label>Number to insert<div class="toolbar"><input type="number" step="any" data-formula-constant aria-label="Formula constant"><button type="button" class="pixel-button small" data-insert-constant>Insert number</button></div></label><label>Readable expression<output data-formula-readable></output></label><details class="formula-advanced"><summary>Advanced: edit stable-ID expression</summary><label>Internal expression<textarea name="expression" required maxlength="4096" placeholder="Choose fields above">${esc(options.expression||'')}</textarea></label></details><label>Relationship to aggregate (optional)<select name="aggregation_relationship"><option value="">No aggregation</option>${relationships.map(field=>`<option value="${esc(field.id)}" ${aggregation.relationship_field_id===field.id?'selected':''}>${esc(field.name)}</option>`).join('')}</select></label><label>Aggregation operation<select name="aggregation_operation">${['sum','avg','min','max','count'].map(op=>`<option value="${op}" ${aggregation.operation===op?'selected':''}>${op}</option>`).join('')}</select></label><label>Target numeric field<select name="aggregation_field" data-initial-target="${esc(aggregation.field_id||'')}"></select></label><small>Progress contributes its current value. Missing related values are skipped, as in existing formulas.</small><label>Preview using<select data-formula-object><option value="">Temporary example values</option>${state.objects.filter(object=>object.object_type_id===typeFields[0]?.object_type_id).map(object=>`<option value="${esc(object.id)}">${esc(object.name)}</option>`).join('')}</select></label><div class="formula-example-inputs">${typeFields.filter(field=>!field.archived&&['integer','decimal','percentage','progress'].includes(field.field_type)).map(field=>`<label>${esc(field.name)}${field.field_type==='progress'?' (current)':''}<input type="number" step="any" data-formula-example="${esc(field.id)}" data-example-progress="${field.field_type==='progress'}" value="${esc(field.field_type==='progress'?field.default_value?.current??'':field.default_value??'')}"></label>`).join('')}<label>Temporary relationship result<input type="number" step="any" data-formula-related></label></div><output class="formula-status" role="status" data-formula-status>Choose fields to build your expression.</output></div>`;
}

export function bindFormulaEditor(form,type,state,editingID='') {
  let timer,sequence=0;
  const expression=form.elements.expression;
  if(!expression)return;
  const builder=form.querySelector('.formula-builder');
  const status=builder.querySelector('[data-formula-status]');
  const readable=builder.querySelector('[data-formula-readable]');
  const updateTargets=()=>{
    const relationship=type.fields.find(field=>field.id===form.elements.aggregation_relationship.value);
    const select=form.elements.aggregation_field;
    const previous=select.value||select.dataset.initialTarget;delete select.dataset.initialTarget;
    const allowedType=relationship?.options?.allowed_type_id;
    const targets=state.types.filter(item=>!item.archived&&(!allowedType||item.id===allowedType)).flatMap(item=>(item.fields||[]).filter(field=>!field.archived&&['integer','decimal','percentage','progress'].includes(field.field_type)).map(field=>({...field,type_name:item.name})));
    select.innerHTML='<option value="">Choose target field</option>'+targets.map(field=>`<option value="${esc(field.id)}" ${field.id===previous?'selected':''}>${esc(field.type_name)} · ${esc(field.name)}</option>`).join('');
    select.disabled=!relationship||form.elements.aggregation_operation.value==='count';
  };
  updateTargets();
  const preview=async()=>{
    if(!builder.isConnected||form.elements.expression!==expression)return;
    const version=++sequence;
    readable.textContent=expression.value.replace(/@\{([^}]+)\}|@([^\s+*/()\-]+)/g,(_,braced,bare)=>{
      const id=braced||bare;if(id==='related')return '[Relationship result]';
      const field=type.fields.find(item=>item.id===id);
      return field?`[${field.name}${field.archived?' — archived':''}]`:'[Missing field]';
    })||'Your formula will appear here.';
    const objectID=builder.querySelector('[data-formula-object]').value;
    const examples=builder.querySelector('.formula-example-inputs');examples.hidden=Boolean(objectID);
    if(!expression.value.trim()){status.textContent='Choose a field or enter an expression.';return;}
    status.textContent='Validating with the server…';
    const options={expression:expression.value};
    if(form.elements.aggregation_relationship.value)options.aggregation={relationship_field_id:form.elements.aggregation_relationship.value,operation:form.elements.aggregation_operation.value,field_id:form.elements.aggregation_field.value};
    const values={};
    if(!objectID)for(const input of builder.querySelectorAll('[data-formula-example]'))if(input.value!=='')values[input.dataset.formulaExample]=input.dataset.exampleProgress==='true'?{current:Number(input.value),max:Math.max(1,Number(input.value))}:Number(input.value);
    const related=builder.querySelector('[data-formula-related]').value;
    try {
      const result=await request(`/api/object-types/${type.id}/formula-preview`,{method:'POST',body:{field_id:editingID,object_id:objectID,options,values,...(related!==''?{related:Number(related)}:{})}});
      if(version!==sequence||!builder.isConnected)return;
      status.textContent=result.error?`Valid formula; example cannot calculate: ${result.error}`:`Valid formula · Example result: ${result.result}`;
      status.classList.toggle('formula-error',Boolean(result.error));
    }catch(error){if(version===sequence&&builder.isConnected){status.textContent=error.message;status.classList.add('formula-error');}}
  };
  const schedule=()=>{clearTimeout(timer);sequence++;timer=setTimeout(preview,300);};
  builder.addEventListener('input',schedule);
  builder.addEventListener('change',event=>{
    if(['aggregation_relationship','aggregation_operation'].includes(event.target.name))updateTargets();schedule();
  });
  builder.addEventListener('click',event=>{
    const insert=event.target.closest('[data-formula-insert],[data-insert-constant]');if(!insert)return;
    const token=insert.dataset.formulaInsert??builder.querySelector('[data-formula-constant]').value;
    if(token==='')return;
    const start=expression.selectionStart??expression.value.length,end=expression.selectionEnd??start;
    expression.setRangeText(` ${token} `,start,end,'end');expression.dispatchEvent(new Event('input',{bubbles:true}));schedule();
  });
  expression.addEventListener('invalid',()=>{builder.querySelector('details').open=true;});
  preview();
}
