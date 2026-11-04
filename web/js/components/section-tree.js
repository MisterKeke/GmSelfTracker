import { request } from '../api.js';
import { closeModal, ensureDialog } from '../modal.js';
import { esc, renderIcon } from '../utils/formatting.js';

import { descendants, sectionRows } from '../utils/section-tree-model.js';
export { descendants, sectionRows } from '../utils/section-tree-model.js';

export function createSectionTree({state,root,load,render,openForm,toast,run}) {
  let draggedID = null;
  let hintedItem = null;
  function dropMode(event,item) {
    if(event.target.closest('[data-drop-before]'))return 'before';
    const row=item?.querySelector('.tree-row');if(!row)return 'inside';
    const rect=row.getBoundingClientRect(),ratio=(event.clientY-rect.top)/rect.height;
    return ratio<0.25?'before':ratio>0.75?'after':'inside';
  }
  function renderSectionTree() {
    const rows = sectionRows(state.sections,state.expanded);
    return `<nav class="section-tree" aria-label="Sections"><div class="panel-heading"><strong>▣ SECTIONS</strong><button class="pixel-button small" type="button" data-action="new-section" aria-label="Create section">＋</button></div><p class="tree-hint">Drag grips to row edges to reorder, or the center to nest. Use Move for keyboard controls.</p><div role="tree" aria-label="Game sections">${rows.map(({section,depth,hasChildren}) => {
      const selected = state.panel==='section' && state.sectionId===section.id;
      return `<div class="tree-item ${selected?'selected':''}" role="treeitem" aria-level="${depth+1}" aria-selected="${selected}" ${hasChildren?`aria-expanded="${state.expanded.has(section.id)}"`:''} data-section-id="${esc(section.id)}" style="--tree-depth:${depth}"><div class="tree-drop-line" data-drop-before="${esc(section.id)}" aria-hidden="true"></div><div class="tree-row"><span class="tree-grip" draggable="true" data-drag-section="${esc(section.id)}" title="Drag to organize" aria-hidden="true">⠿</span><button type="button" class="tree-toggle" data-action="toggle-branch" data-id="${esc(section.id)}" aria-label="${hasChildren?(state.expanded.has(section.id)?'Collapse':'Expand'):'No children in'} ${esc(section.name)}" ${hasChildren?'':'disabled'}>${hasChildren?(state.expanded.has(section.id)?'▼':'▶'):'·'}</button><button type="button" class="tree-select" data-action="select-section" data-id="${esc(section.id)}" ${selected?'aria-current="page"':''}>${renderIcon(section.icon)} <span>${esc(section.name)}</span></button><button type="button" class="tree-move" data-action="move-section" data-id="${esc(section.id)}" aria-label="Move ${esc(section.name)}">↕</button></div></div>`;
    }).join('')}</div>${rows.length?'':'<p class="search-empty">No active sections. Create your first branch.</p>'}<div class="tree-root-drop" data-drop-root>Drop here to move to root</div></nav>`;
  }

  async function moveSection(id,parent_id,before_id='') {
    await request(`/api/sections/${id}/move`,{method:'POST',body:{parent_id,before_id}});
    if (parent_id) state.expanded.add(parent_id);
    await load(); toast('Section moved.');
  }

  function openMoveSection(id) {
    const section=state.sections.find(item=>item.id===id);
    if (!section) return;
    const excluded=descendants(state.sections,id);
    openForm('MOVE SECTION',`<p class="dialog-help wide">Move ${esc(section.name)} with its children. Objects and goals keep their placements.</p><label class="form-field wide">Parent section<select name="parent_id" data-move-parent><option value="">Top level</option>${state.sections.filter(item=>!item.archived&&!excluded.has(item.id)).map(item=>`<option value="${esc(item.id)}" ${item.id===section.parent_id?'selected':''}>${esc(item.name)}</option>`).join('')}</select></label><label class="form-field wide">Position<select name="before_id" data-move-before></select></label><input type="hidden" name="section_id" value="${esc(id)}"><div class="toolbar wide"><button type="button" class="pixel-button small" data-action="section-up" data-id="${esc(id)}">Move up</button><button type="button" class="pixel-button small" data-action="section-down" data-id="${esc(id)}">Move down</button><button type="button" class="pixel-button small" data-action="section-root" data-id="${esc(id)}">Move to root</button></div>`,'MOVE');
    const form=ensureDialog(root).querySelector('form');form.dataset.form='move-section';
    const update=()=>{
      const parent=form.elements.parent_id.value || null;
      form.elements.before_id.innerHTML='<option value="">Last</option>'+state.sections.filter(item=>!item.archived&&item.parent_id===parent&&item.id!==id).sort((a,b)=>a.sort_order-b.sort_order||a.name.localeCompare(b.name)).map(item=>`<option value="${esc(item.id)}">Before ${esc(item.name)}</option>`).join('');
    };
    form.elements.parent_id.addEventListener('change',update);update();
  }

  async function shiftSection(id,direction) {
    const section=state.sections.find(item=>item.id===id);
    const siblings=state.sections.filter(item=>!item.archived&&item.parent_id===section.parent_id).sort((a,b)=>a.sort_order-b.sort_order||a.name.localeCompare(b.name)||a.id.localeCompare(b.id));
    const position=siblings.findIndex(item=>item.id===id);
    if (direction<0 && position>0) await moveSection(id,section.parent_id,siblings[position-1].id);
    else if (direction>0 && position<siblings.length-1) await moveSection(id,section.parent_id,siblings[position+2]?.id||'');
  }

  root.addEventListener('click',event=>{
    const button=event.target.closest('[data-action]');if(!button)return;
    const id=button.dataset.id;
    if(button.dataset.action==='toggle-branch') {state.expanded.has(id)?state.expanded.delete(id):state.expanded.add(id);render();root.querySelector(`[data-section-id="${id}"] .tree-toggle`)?.focus();}
    if(button.dataset.action==='move-section') openMoveSection(id);
    if(['section-up','section-down','section-root'].includes(button.dataset.action)) run(async()=>{
      if(button.dataset.action==='section-root')await moveSection(id,null);else await shiftSection(id,button.dataset.action==='section-up'?-1:1);
      closeModal(ensureDialog(root));
    });
  });
  root.addEventListener('keydown',event=>{
    const button=event.target.closest('.tree-select');if(!button)return;
    const buttons=[...root.querySelectorAll('.tree-select')],index=buttons.indexOf(button);
    const row=button.closest('.tree-item'),id=row.dataset.sectionId;
    if(['ArrowDown','ArrowUp','Home','End'].includes(event.key)) {
      event.preventDefault();buttons[event.key==='Home'?0:event.key==='End'?buttons.length-1:Math.max(0,Math.min(buttons.length-1,index+(event.key==='ArrowDown'?1:-1)))]?.focus();
    } else if(event.key==='ArrowRight'||event.key==='ArrowLeft') {
      event.preventDefault();
      if(event.key==='ArrowRight')state.expanded.add(id);else state.expanded.delete(id);
      render();root.querySelector(`[data-section-id="${id}"] .tree-select`)?.focus();
    }
  });
  root.addEventListener('dragstart',event=>{
    const row=event.target.closest('[data-drag-section]');if(!row)return;
    draggedID=row.dataset.dragSection;event.dataTransfer.setData('text/plain',draggedID);event.dataTransfer.effectAllowed='move';
  });
  root.addEventListener('dragover',event=>{
    if(draggedID&&event.target.closest('.tree-item,[data-drop-root]')) {
      event.preventDefault();event.dataTransfer.dropEffect='move';
      const item=event.target.closest('.tree-item');
      if(hintedItem!==item)hintedItem?.removeAttribute('data-drop-hint');
      hintedItem=item;if(item)item.dataset.dropHint=dropMode(event,item);
    }
  });
  root.addEventListener('drop',event=>{
    if(!draggedID)return;
    const id=draggedID;draggedID=null;
    const before=event.target.closest('[data-drop-before]');
    const target=event.target.closest('[data-section-id]');
    if(!before&&!target&&!event.target.closest('[data-drop-root]'))return;
    event.preventDefault();
    const section=state.sections.find(item=>item.id===(before?.dataset.dropBefore||target?.dataset.sectionId));
    const mode=target?dropMode(event,target):'inside';
    const parent=section?(mode==='inside'?section.id:section.parent_id):null;
    let beforeID=mode==='before'?section?.id||'':'';
    if(mode==='after'&&section){
      const siblings=state.sections.filter(item=>!item.archived&&item.parent_id===section.parent_id&&item.id!==id).sort((a,b)=>a.sort_order-b.sort_order||a.name.localeCompare(b.name)||a.id.localeCompare(b.id));
      beforeID=siblings[siblings.findIndex(item=>item.id===section.id)+1]?.id||'';
    }
    hintedItem?.removeAttribute('data-drop-hint');hintedItem=null;
    if(descendants(state.sections,id).has(parent)||section?.id===id) {toast('A section cannot be moved inside itself or a descendant.',true);return;}
    run(()=>moveSection(id,parent,beforeID));
  });
  root.addEventListener('dragend',()=>{draggedID=null;hintedItem?.removeAttribute('data-drop-hint');hintedItem=null;});
  return {renderSectionTree,moveSection,openMoveSection,shiftSection};
}
