import { getItems } from '../api.js';
import { ensureDialog } from '../modal.js';
import { esc, renderIcon } from '../utils/formatting.js';

export function createAccountEditor({ openForm, fieldRow, state, root }) {
  async function openProfile() {
    const images=await getItems('/api/assets');
    const imageManager=`<div class="form-field wide"><label>Uploaded images</label><small>${images.length} / 1000 images · ${Math.round(images.reduce((sum,image)=>sum+image.size_bytes,0)/1024/1024)} / 250 MB. Images in use cannot be deleted.</small><div class="asset-list">${images.map((image)=>`<div class="section-row">${image.pending_deletion?'<span>Cleanup pending</span>':`<img class="custom-icon" src="${esc(image.url)}" alt="Uploaded image" loading="lazy">`}<small>${Math.ceil(image.size_bytes/1024)} KB</small><button class="pixel-button small danger" type="button" data-action="delete-image" data-id="${esc(image.id)}">${image.pending_deletion?'Retry cleanup':'Delete'}</button></div>`).join('')}</div></div>`;
    const allGames=await getItems('/api/games?archived=true');
    const archivedGames=allGames.filter((item)=>item.archived);
    const archivedObjects=state.game?await getItems(`/api/games/${state.game.id}/objects?archived=true`):[];
    const archivedSections=state.sections.filter((item)=>item.archived);
    const archivedItems=[
      ...archivedGames.map((item)=>`<div class="section-row"><span>🎮</span><span>${esc(item.name)}</span><button type="button" class="pixel-button small" data-action="restore-game" data-id="${esc(item.id)}">Restore</button></div>`),
      ...archivedSections.map((item)=>`<div class="section-row"><span>${renderIcon(item.icon)}</span><span>${esc(item.name)} <small>section</small></span><button type="button" class="pixel-button small" data-action="restore-section" data-id="${esc(item.id)}">Restore</button></div>`),
      ...archivedObjects.filter((item)=>item.archived).map((item)=>`<div class="section-row"><span>◈</span><span>${esc(item.name)} <small>${esc(item.type_name)}</small></span><button type="button" class="pixel-button small" data-action="restore-object" data-id="${esc(item.id)}">Restore</button></div>`),
    ];
    const archives=archivedItems.length?`<div class="form-field wide"><label>Archived data</label><div class="section-list light-panel">${archivedItems.join('')}</div></div>`:`<p class="dialog-help wide">There is no archived data to restore right now.</p>`;
    openForm('PROFILE & SETTINGS',`<p><strong>Username:</strong> ${esc(state.user.username)}</p><p class="dialog-help">Your account and game data are stored in the local SQLite database.</p><div class="form-field wide"><label>Change password</label></div>${fieldRow('Current password','current_password','','password',true,'required autocomplete="current-password"')}${fieldRow('New password','password','','password',true,'required minlength="10" maxlength="72" autocomplete="new-password"')}${fieldRow('Confirm new password','confirm_password','','password',true,'required minlength="10" maxlength="72" autocomplete="new-password"')}${archives}${imageManager}`, 'UPDATE PASSWORD');
    ensureDialog(root).querySelector('#modal-form').dataset.form='change-password';
  }

  return { openProfile };
}
