import { closeModal, ensureDialog } from '../modal.js';
import { request } from '../api.js';
import { esc } from '../utils/formatting.js';

export function createRelationshipGraphEditor({ openForm, openObjectForm, state, root }) {
  let graph=null;
  let rootObject=null;
  let zoom=1;
  let pan={x:0,y:0};
  let error='';
  const collapsedNodes=new Set();

  async function openObjectGraph(objectID) {
    rootObject=state.objectCache.get(objectID)||state.objects.find(item=>item.id===objectID)||await request(`/api/objects/${encodeURIComponent(objectID)}`);
    graph=await request(`/api/objects/${encodeURIComponent(objectID)}/graph?depth=1&max_nodes=50&direction=both`);
    zoom=1;pan={x:0,y:0};error='';renderDialog();
  }

  function renderDialog() {
    openForm(`RELATIONSHIP GRAPH · ${rootObject.name}`,graphMarkup(),'CLOSE');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form='close-modal';
    form.addEventListener('submit',event=>{event.preventDefault();event.stopPropagation();closeModal(ensureDialog(root));});
    form.addEventListener('click',event=>{
      const toggle=event.target.closest('[data-graph-toggle]');
      if(toggle){const id=toggle.dataset.graphToggle;if(collapsedNodes.has(id)){collapsedNodes.delete(id);renderDialog();}else if(graph.nodes.some(item=>item.parent_ids?.includes(id))){collapsedNodes.add(id);renderDialog();}else expandNode(id);return;}
      const open=event.target.closest('[data-graph-open]');
      if(open){loadObject(open.dataset.graphOpen);return;}
      const select=event.target.closest('[data-graph-select]');
      if(select){graph.selectedNodeID=select.dataset.graphSelect;renderDialog();return;}
      const center=event.target.closest('[data-graph-center]');
      if(center){centerNode(center.dataset.graphCenter,form);return;}
      const zoomButton=event.target.closest('[data-graph-zoom]');
      if(zoomButton){const value=zoomButton.dataset.graphZoom;if(value==='reset'){zoom=1;pan={x:0,y:0};}else zoom=Math.max(.5,Math.min(2.5,zoom+(value==='in'?.2:-.2)));applyViewBox(form);}
    });
    const svg=form.querySelector('[data-relationship-svg]');let pointer=null;
    svg.addEventListener('click',event=>{const node=event.target.closest('[data-node-id]');if(!node)return;graph.selectedNodeID=node.dataset.nodeId;svg.querySelectorAll('[data-node-id]').forEach(item=>item.classList.toggle('is-selected',item.dataset.nodeId===graph.selectedNodeID));});
    svg.addEventListener('pointerdown',event=>{if(event.target.closest('[data-node-id]'))return;pointer={x:event.clientX,y:event.clientY,panX:pan.x,panY:pan.y};svg.setPointerCapture?.(event.pointerId);});
    svg.addEventListener('pointermove',event=>{if(!pointer)return;const rect=svg.getBoundingClientRect();pan.x=pointer.panX-(event.clientX-pointer.x)*(graph.canvasWidth*zoom/rect.width);pan.y=pointer.panY-(event.clientY-pointer.y)*(graph.canvasHeight*zoom/rect.height);applyViewBox(form);});
    svg.addEventListener('pointerup',()=>{pointer=null;});svg.addEventListener('pointercancel',()=>{pointer=null;});
    applyViewBox(form);
  }

  async function expandNode(objectID) {
    const node=graph.nodes.find(item=>item.id===objectID);if(!node||node.archived)return;
    try{
      const next=await request(`/api/objects/${encodeURIComponent(objectID)}/graph?depth=1&max_nodes=100&direction=both`);
      const nodes=new Map(graph.nodes.map(item=>[item.id,item]));
      for(const item of next.nodes){if(item.id===objectID)continue;const existing=nodes.get(item.id);if(existing){const parents=new Set([...(existing.parent_ids||[]),objectID,...(item.parent_ids||[]).filter(parent=>parent!==objectID)]);existing.parent_ids=[...parents];nodes.set(item.id,existing);continue;}if(nodes.size>=100){graph.truncated=true;continue;}nodes.set(item.id,{...item,depth:node.depth+1,parent_ids:[...(item.parent_ids||[]).filter(parent=>parent!==objectID),objectID]});}
      const edges=new Map(graph.edges.map(item=>[`${item.source}:${item.field_id}:${item.target}`,item]));
      for(const item of next.edges){const key=`${item.source}:${item.field_id}:${item.target}`;if(!edges.has(key)){if(edges.size>=200){graph.truncated=true;continue;}edges.set(key,item);}else if(edges.get(key).reverse&&!item.reverse)edges.set(key,item);}
      graph.nodes=[...nodes.values()];graph.edges=[...edges.values()];graph.truncated=graph.truncated||next.truncated;error='';renderDialog();
    }catch(ex){error=ex.message;renderDialog();}
  }

  async function loadObject(objectID) {
    try{const object=await request(`/api/objects/${encodeURIComponent(objectID)}`);closeModal(ensureDialog(root));openObjectForm(object);}
    catch(ex){error=ex.message;renderDialog();}
  }

  function graphMarkup() {
    const visible=visibleGraph();const positions=graphPositions(visible);graph.canvasWidth=visible.canvasWidth;graph.canvasHeight=visible.canvasHeight;const edges=visible.edges.map(edge=>{
      const from=positions.get(edge.source),to=positions.get(edge.target);if(!from||!to)return '';
      const middle=(from.x+to.x)/2;const labelX=middle,labelY=(from.y+to.y)/2-7;
      return `<path class="relationship-edge ${edge.reverse?'is-reverse':''}" d="M ${from.x} ${from.y} Q ${middle} ${from.y} ${to.x} ${to.y}" marker-end="url(#relationship-arrow)"><title>${esc(edge.field_name)}${edge.reverse?' · reached through a reverse reference':''}</title></path><text class="relationship-edge-label" x="${labelX}" y="${labelY}">${esc(edge.field_name)}${edge.reverse?' ↶':''}</text>`;
    }).join('');
    const nodes=visible.nodes.map(node=>{const point=positions.get(node.id),root=node.id===graph.root_id;return `<g class="relationship-node ${root?'is-root':''} ${node.archived?'is-archived':''} ${node.id===graph.selectedNodeID?'is-selected':''}" data-node-id="${esc(node.id)}" transform="translate(${point.x-78} ${point.y-34})"><rect width="156" height="68" rx="4"></rect><text x="78" y="27" text-anchor="middle">${esc(node.name.length>22?`${node.name.slice(0,21)}…`:node.name)}</text><text x="78" y="48" text-anchor="middle" class="relationship-type-label">${esc(node.archived?'Archived · ':'')}${esc(node.type_name)}</text></g>`;}).join('');
    const nodeRows=visible.nodes.map(node=>{const collapsed=collapsedNodes.has(node.id),hasChildren=graph.nodes.some(item=>item.parent_ids?.includes(node.id));return `<li><span><strong>${esc(node.name)}</strong> · ${esc(node.type_name)}${node.archived?' · archived':''}</span><div class="toolbar"><button class="pixel-button small" type="button" data-graph-select="${esc(node.id)}" ${node.id===graph.selectedNodeID?'aria-pressed="true"':''}>Select</button><button class="pixel-button small" type="button" data-graph-open="${esc(node.id)}">Open object</button><button class="pixel-button small" type="button" data-graph-center="${esc(node.id)}">Center</button><button class="pixel-button small" type="button" data-graph-toggle="${esc(node.id)}" ${node.archived?'disabled':''}>${collapsed?'Expand branch':hasChildren?'Collapse branch':'Expand related'}</button></div></li>`;}).join('');
    return `<div class="relationship-graph-toolbar"><div class="toolbar"><button class="pixel-button small" type="button" data-graph-zoom="out" aria-label="Zoom out">−</button><button class="pixel-button small" type="button" data-graph-zoom="in" aria-label="Zoom in">＋</button><button class="pixel-button small" type="button" data-graph-zoom="reset">Reset view</button></div><span>${visible.nodes.length} visible · ${graph.nodes.length} loaded nodes · ${visible.edges.length} relationships</span></div>${error?`<p class="form-error" role="alert">${esc(error)}</p>`:''}${graph.truncated?'<p class="dashboard-warning" role="status">The graph reached its visible node or edge limit. Expand individual nodes to continue exploring.</p>':''}<svg class="relationship-graph-svg" data-relationship-svg viewBox="0 0 ${graph.canvasWidth||1200} ${graph.canvasHeight||800}" role="img" aria-label="Relationship graph for ${esc(rootObject.name)}"><defs><marker id="relationship-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z"></path></marker></defs>${edges}${nodes}</svg><p class="dialog-help">Edges follow the actual reference direction. Dashed edges were reached by following a reference back toward this object. Click a node to select it; drag the canvas to pan.</p><ul class="relationship-node-list">${nodeRows}</ul>`;
  }

  function visibleGraph(){const byID=new Map(graph.nodes.map(node=>[node.id,node])),visible=new Set([graph.root_id]),queue=[graph.root_id];for(let index=0;index<queue.length;index++){const parent=queue[index];if(collapsedNodes.has(parent))continue;for(const node of graph.nodes){if(!node.parent_ids?.includes(parent)||visible.has(node.id))continue;visible.add(node.id);queue.push(node.id);}}return {...graph,nodes:graph.nodes.filter(node=>visible.has(node.id)),edges:graph.edges.filter(edge=>visible.has(edge.source)&&visible.has(edge.target)),_byID:byID};}

  function centerNode(objectID,form){const visible=visibleGraph(),positions=graphPositions(visible),point=positions.get(objectID);if(!point)return;pan.x=point.x-graph.canvasWidth/2;pan.y=point.y-graph.canvasHeight/2;applyViewBox(form);}

  function graphPositions(current) {
    const maxDepth=Math.max(1,...current.nodes.map(node=>node.depth));const canvasWidth=Math.max(1200,200+maxDepth*440);const groups=new Map();
    for(const node of current.nodes){const side=node.reverse_expansion?'r':'f';const key=`${node.depth}:${side}`;if(!groups.has(key))groups.set(key,[]);groups.get(key).push(node);}
    const maxLayer=Math.max(1,...[...groups.values()].map(group=>group.length));const canvasHeight=Math.max(700,maxLayer*110+100);current.canvasWidth=canvasWidth;current.canvasHeight=canvasHeight;
    const centerX=canvasWidth/2,centerY=canvasHeight/2,positions=new Map();
    for(const [key,items] of groups){items.sort((a,b)=>a.name.localeCompare(b.name));const [depth,side]=key.split(':');for(let index=0;index<items.length;index++){const node=items[index];if(Number(depth)===0){positions.set(node.id,{x:centerX,y:centerY});continue;}const x=centerX+(side==='r'?-1:1)*Number(depth)*210;const y=(canvasHeight-(items.length-1)*105)/2+index*105;positions.set(node.id,{x,y});}}
    return positions;
  }

  function applyViewBox(form){const svg=form.querySelector('[data-relationship-svg]');if(!svg)return;const width=graph.canvasWidth*zoom,height=graph.canvasHeight*zoom,x=(graph.canvasWidth-width)/2+pan.x,y=(graph.canvasHeight-height)/2+pan.y;svg.setAttribute('viewBox',`${x} ${y} ${width} ${height}`);}
  return { openObjectGraph };
}
