export function createWorkspaceState(user) {
  return { user, games: [], game: null, sections: [], types: [], objects: [], objectCache:new Map(), objectTotal: 0, gameObjectTotal: 0, objectQueryError: '', goals: [], sectionGoals: [], goalCache:new Map(), goalStats:{total:0,completed:0}, goalQueryItems: [], goalQueryTotal: 0, goalQueryError: '', goalQueryLoading: false, panel: 'overview', sectionId: null, search: '', objectPage: 0, goalPage: 0, goalFilters: {search:'',status:'all',type:'all',sort:'created_at',direction:'desc'}, expanded: new Set(), views: new Map() };
}
