import { sectionDisplayConfig, prettyValue, formatNumber, esc, renderIcon } from '../utils/formatting.js';

export function createObjectViews({ state }) {
  function renderObjectList(objects){
    return `<div class="object-list-view">${objects.map((object)=>`<article class="object-list-row light-panel"><div class="list-object-symbol">${object.image?`<img src="${esc(object.image)}" alt="">`:'◈'}</div><div class="list-object-main"><strong>${esc(object.name)}</strong><small>${esc(object.type_name)}</small><div class="list-object-values">${(object.fields||[]).filter((field)=>!field.archived).slice(0,2).map((field)=>`<span>${renderIcon(field.icon||'✦')} ${esc(field.name)}: <b>${esc(prettyValue(object.values?.[field.id],field,state.objects))}</b></span>`).join('')}</div></div><div class="toolbar"><button class="pixel-button small" type="button" data-action="edit-object" data-id="${esc(object.id)}">Edit</button><button class="pixel-button small soft" type="button" data-action="remove-placement" data-id="${esc(object.id)}">Remove here</button></div></article>`).join('')}</div>`;
  }

  function renderObjectTable(objects,section){
    const config=sectionDisplayConfig(section);
    const configured=new Set(config.visible_field_ids||[]);
    const columns=[];for(const object of objects){for(const field of (object.fields||[]).filter((item)=>!item.archived)){if((configured.size===0||configured.has(field.id))&&!columns.some((item)=>item.id===field.id)&&columns.length<12)columns.push(field);}}
    return `<div class="table-scroll light-panel"><table class="object-table"><thead><tr><th>Object</th><th>Type</th>${columns.map((field)=>`<th>${renderIcon(field.icon||'✦')} ${esc(field.name)}</th>`).join('')}<th>Actions</th></tr></thead><tbody>${objects.map((object)=>`<tr><td><strong>${esc(object.name)}</strong></td><td>${esc(object.type_name)}</td>${columns.map((field)=>`<td>${object.object_type_id===field.object_type_id?esc(prettyValue(object.values?.[field.id],field,state.objects)):'—'}</td>`).join('')}<td><button class="pixel-button small" type="button" data-action="edit-object" data-id="${esc(object.id)}">Edit</button> <button class="pixel-button small soft" type="button" data-action="remove-placement" data-id="${esc(object.id)}">Remove</button></td></tr>`).join('')}</tbody></table></div>`;
  }

  function renderChecklistView(objects,section){
    const fieldID=sectionDisplayConfig(section).checklist_field_id;
    const field=state.types.flatMap((type)=>type.fields||[]).find((item)=>item.id===fieldID&&!item.archived&&item.field_type==='boolean');
    if(!field)return `<div class="empty-state"><div class="empty-icon">☑</div><h3>CHOOSE A CHECKBOX FIELD</h3><p>Edit this section and select one of your custom checkbox fields to turn its objects into a checklist.</p><button class="pixel-button" type="button" data-action="edit-section" data-id="${esc(section.id)}">Configure checklist</button></div>`;
    const compatible=objects.filter((object)=>object.object_type_id===field.object_type_id);
    return `<section class="light-panel task-list section-checklist"><div class="panel-heading"><strong>${renderIcon(field.icon||'☑')} ${esc(field.name)}</strong><span>${compatible.filter((object)=>Boolean(object.values?.[field.id])).length} / ${compatible.length} complete</span></div>${compatible.map((object)=>`<div class="task-line"><input aria-label="${esc(object.name)}: ${esc(field.name)}" type="checkbox" data-action="toggle-object-checklist" data-object-id="${esc(object.id)}" data-field-id="${esc(field.id)}" ${object.values?.[field.id]?'checked':''}><span><strong>${esc(object.name)}</strong><small>${esc(object.type_name)}</small></span><button class="pixel-button small soft" type="button" data-action="edit-object" data-id="${esc(object.id)}">Edit</button></div>`).join('')||'<p class="empty-state">No objects of this field type are placed here yet.</p>'}</section>`;
  }

  function renderProgressView(objects,section){
    const config=sectionDisplayConfig(section);
    const rows=objects.map((object)=>({object,value:objectProgress(object,config)})).filter((row)=>row.value!==null);
    if(!rows.length)return `<div class="empty-state"><h3>CONFIGURE PROGRESS</h3><p>Choose a progress field, calculate from multiple numeric fields, or sum progress from linked child objects.</p><button class="pixel-button" type="button" data-action="edit-section" data-id="${esc(section.id)}">Configure progress</button></div>`;
    return `<section class="light-panel section-progress-list"><div class="panel-heading"><strong>Progress</strong><span>${esc(config.progress_mode||'field')} · current / maximum</span></div>${rows.map(({object,value})=>{const {current,max}=value;const percent=max>0?Math.max(0,Math.min(100,current/max*100)):0;return `<article class="section-progress-row"><div><strong>${esc(object.name)}</strong><small>${max>0?`${esc(formatNumber(current))} / ${esc(formatNumber(max))}`:'Missing values or maximum'}</small></div><div class="progress-track" role="progressbar" aria-valuenow="${Math.round(percent)}" aria-valuemin="0" aria-valuemax="100" aria-label="${esc(object.name)} progress"><div class="progress-fill" style="width:${percent}%"></div></div><b>${max>0?`${Math.round(percent)}%`:'—'}</b><button class="pixel-button small" type="button" data-action="edit-object" data-id="${esc(object.id)}">Edit</button></article>`;}).join('')}</section>`;
  }

  function objectProgress(object,config){
    const mode=config.progress_mode||'field';
    if(mode==='calculated'){
      const currentFields=config.progress_current_field_ids||[];const maximumFields=config.progress_max_field_ids||[];
      if(!currentFields.length||![...currentFields,...maximumFields].every((id)=>object.fields?.some((field)=>field.id===id&&!field.archived)))return null;
      const sum=(ids)=>{const numbers=ids.map((id)=>object.values?.[id]);return numbers.every((n)=>typeof n==='number'&&Number.isFinite(n))?numbers.reduce((a,b)=>a+b,0):NaN;};
      const current=sum(currentFields);const max=maximumFields.length?sum(maximumFields):Number(config.progress_maximum);
      return Number.isFinite(current)&&Number.isFinite(max)&&max>0?{current,max}:{current:0,max:0};
    }
    if(mode==='children'){
      const field=object.fields?.find((item)=>item.id===config.progress_relationship_field_id&&!item.archived&&['reference','references'].includes(item.field_type));
      if(!field)return null;
      const raw=object.values?.[field.id];const ids=Array.isArray(raw)?raw:[raw].filter(Boolean);
      let current=0,max=0;
      for(const id of new Set(ids)){const child=state.objects.find((item)=>item.id===id);const value=child?.values?.[config.progress_child_field_id];if(value&&Number.isFinite(value.current)&&Number.isFinite(value.max)&&value.max>0){current+=value.current;max+=value.max;}}
      return Number.isFinite(current)&&Number.isFinite(max)?{current,max}:{current:0,max:0};
    }
    if(!object.fields?.some((field)=>field.id===config.progress_field_id&&!field.archived&&field.field_type==='progress'))return null;
    const value=object.values?.[config.progress_field_id];return value&&Number.isFinite(value.current)&&Number.isFinite(value.max)?value:{current:0,max:0};
  }

  function renderObjectCard(object,section=null) {
    const config=section?sectionDisplayConfig(section):{};
    const selected=new Set(config.visible_field_ids||[]);
    const available=(object.fields||[]).filter((field)=>!field.archived);
    const fields = (selected.size?available.filter((field)=>selected.has(field.id)):available).slice(0, 6);
    const primaryID=state.types.find(type=>type.id===object.object_type_id)?.primary_display_field_id;
    const primaryField=available.find(field=>field.id===primaryID&&field.object_type_id===object.object_type_id&&(!selected.size||selected.has(field.id)));
    const primaryValue=primaryField?(object.formula_errors?.[primaryField.id]?`⚠ ${object.formula_errors[primaryField.id]}`:prettyValue(object.values?.[primaryField.id],primaryField,state.objects)):'';
    const imageURL=object.image||state.types.find((item)=>item.id===object.object_type_id)?.image;
    const image = imageURL ? `<img src="${esc(imageURL)}" alt="">` : '';
    return `<article class="object-card">
      <div class="object-art">${image}<span class="object-art-symbol" ${imageURL ? 'hidden' : ''}>${esc(object.type_name?.slice(0,1)?.toUpperCase() || '✦')}</span>${primaryField ? `<span class="object-level">${esc(primaryField.name)}: ${esc(primaryValue)}</span>` : ''}</div>
      <div class="object-details"><div class="object-title-row"><div><h3 class="object-title">${esc(object.name)}</h3><div class="object-type-label">${esc(object.type_name)}</div></div></div>
        ${fields.length ? `<div class="object-fields">${fields.map((field) => {const error=object.formula_errors?.[field.id];return `<div class="field-pair"><span aria-hidden="true">${renderIcon(field.icon || '✦')}</span><span class="field-label">${esc(field.name)}</span><span class="field-value ${error?'formula-error':''}" ${error?`title="${esc(error)}"`:''}>${error?`⚠ ${esc(error)}`:esc(prettyValue(object.values?.[field.id],field,state.objects))}</span></div>`;}).join('')}</div>` : `<p class="object-type-label">No fields defined yet</p>`}
        <div class="object-actions"><button class="pixel-button small" type="button" data-action="edit-object" data-id="${esc(object.id)}">✎ Edit</button>${state.panel === 'section' ? `<button class="pixel-button small soft" type="button" data-action="remove-placement" data-id="${esc(object.id)}">Remove here</button>` : ''}<button class="pixel-button small soft" type="button" data-action="archive-object" data-id="${esc(object.id)}">Archive</button></div>
      </div>
    </article>`;
  }

  return { renderObjectList, renderObjectTable, renderChecklistView, renderProgressView, objectProgress, renderObjectCard };
}
