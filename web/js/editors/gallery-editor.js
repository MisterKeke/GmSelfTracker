import { ensureDialog } from '../modal.js';
import { request } from '../api.js';
import { esc } from '../utils/formatting.js';

export function createGalleryEditor({ openForm, state, root }) {
  async function openObjectGallery(objectOrID) {
    const object=typeof objectOrID==='string'?await request(`/api/objects/${encodeURIComponent(objectOrID)}`):objectOrID;
    const result=await request(`/api/objects/${encodeURIComponent(object.id)}/gallery`);
    const items=result.items||[];
    const readOnly=Boolean(result.archived||object.archived);
    const content=items.length?`<div class="gallery-grid">${items.map((item,index)=>`<article class="gallery-item" data-gallery-item="${esc(item.id)}"><a href="${esc(item.url)}" target="_blank" rel="noopener" aria-label="Open full-size image: ${esc(item.caption||`Gallery image ${index+1}`)}"><img src="${esc(item.url)}" loading="lazy" alt="${esc(item.caption||object.name)}"></a>${readOnly?`<p>${esc(item.caption||'')}</p>`:`<label>Caption<input name="gallery_caption_${esc(item.id)}" maxlength="500" value="${esc(item.caption)}"></label><label>Position<input type="number" name="gallery_order_${esc(item.id)}" min="0" max="10000" value="${item.sort_order}"></label><label class="gallery-primary"><input type="radio" name="primary_gallery_image" value="${esc(item.id)}" ${item.is_primary?'checked':''}> Primary image</label><label class="gallery-remove"><input type="checkbox" data-gallery-remove name="gallery_remove_${esc(item.id)}"> Remove from gallery</label>`}</article>`).join('')}</div>`:'<div class="empty-state">No gallery images yet. Add screenshots or reference images below.</div>';
    const controls=readOnly?'<p class="dashboard-warning" role="status">This object or its type is archived. Restore it before changing its gallery.</p>':`<label class="form-field wide">Add images<input name="gallery_files" type="file" multiple accept="image/png,image/jpeg,image/gif,image/webp,image/bmp"><small>PNG, JPEG, GIF, WebP, or BMP · maximum 5 MB per image.</small></label><p class="dialog-help wide">Images reuse your account uploads. Removing a gallery link does not remove an upload used elsewhere.</p>`;
    openForm(`OBJECT GALLERY · ${object.name}`,`${items.length?`<p class="dialog-help wide">${items.length} / 50 images · click a thumbnail to open full size.</p>${content}`:content}${controls}<input type="hidden" name="object_id" value="${esc(object.id)}">`,readOnly?'CLOSE':'SAVE GALLERY');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=readOnly?'close-modal':'edit-object-gallery';form.dataset.readOnly=String(readOnly);
    return object;
  }
  return { openObjectGallery };
}
