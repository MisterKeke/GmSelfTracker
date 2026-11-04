export const FILTER_OPERATORS = {
  integer: ['eq','ne','gt','gte','lt','lte','between'],
  decimal: ['eq','ne','gt','gte','lt','lte','between'],
  percentage: ['eq','ne','gt','gte','lt','lte','between'],
  formula: ['eq','ne','gt','gte','lt','lte','between'],
  text: ['contains','eq','not_contains','starts_with'],
  long_text: ['contains','eq','not_contains','starts_with'],
  select: ['eq','ne','one_of'],
  boolean: ['checked','unchecked'],
  date: ['before','after','on','between'],
  progress: ['below','above','completed','not_completed'],
  reference: ['eq','ne'], references: ['eq','ne'],
};

export const OPERATOR_LABELS = {eq:'Equals',ne:'Does not equal',gt:'Greater than',gte:'At least',lt:'Less than',lte:'At most',between:'Between',contains:'Contains',not_contains:'Does not contain',starts_with:'Starts with',one_of:'One of',checked:'Checked',unchecked:'Unchecked',before:'Before',after:'After',on:'On',below:'Below %',above:'Above %',completed:'Completed',not_completed:'Not completed'};

const missing = value => value===undefined || value===null || value==='';
export function fieldMatches(object, condition) {
  const field = object.fields?.find(item => item.id===condition.field_id && !item.archived);
  if (!field || !FILTER_OPERATORS[field.field_type]?.includes(condition.operator) || object.formula_errors?.[field.id]) return false;
  const raw=object.values?.[field.id],op=condition.operator,target=condition.value;
  if (missing(raw)) return false;
  if (['integer','decimal','percentage','formula'].includes(field.field_type)) {
    if (typeof raw!=='number' || !Number.isFinite(raw) || typeof target!=='number' || !Number.isFinite(target)) return false;
    return op==='eq'?raw===target:op==='ne'?raw!==target:op==='gt'?raw>target:op==='gte'?raw>=target:op==='lt'?raw<target:op==='lte'?raw<=target:typeof condition.value_to==='number'&&raw>=target&&raw<=condition.value_to;
  }
  if (field.field_type==='progress') {
    if (!Number.isFinite(raw?.current)||!Number.isFinite(raw?.max)||raw.max<=0) return false;
    const percent=raw.current/raw.max*100;
    return op==='completed'?percent>=100:op==='not_completed'?percent<100:typeof target==='number'&&(op==='above'?percent>target:percent<target);
  }
  if (field.field_type==='boolean') return typeof raw==='boolean' && raw===(op==='checked');
  if (field.field_type==='reference'||field.field_type==='references') {
    const contains=(Array.isArray(raw)?raw:[raw]).includes(target);return op==='eq'?contains:!contains;
  }
  if (typeof raw!=='string') return false;
  if (field.field_type==='select') return op==='one_of'?Array.isArray(target)&&target.includes(raw):op==='eq'?raw===target:raw!==target;
  if (field.field_type==='date') return op==='before'?raw<target:op==='after'?raw>target:op==='on'?raw===target:raw>=target&&raw<=condition.value_to;
  if (typeof target!=='string') return false;
  const value=raw.toLocaleLowerCase(),text=target.toLocaleLowerCase();
  return op==='eq'?value===text:op==='contains'?value.includes(text):op==='not_contains'?!value.includes(text):value.startsWith(text);
}

export function queryObjects(objects, view={}) {
  const query=(view.search||'').trim().toLocaleLowerCase();
  const conditions=view.conditions||[];
  const result=objects.filter(object => (!view.type_id||object.object_type_id===view.type_id) &&
    (!query||`${object.name} ${object.type_name||''} ${object.description||''}`.toLocaleLowerCase().includes(query)) &&
    (!conditions.length||(view.mode==='any'?conditions.some(c=>fieldMatches(object,c)):conditions.every(c=>fieldMatches(object,c)))));
  const sort=view.sort||'name',direction=view.direction==='desc'?-1:1;
  const value=object=>{
    if (['name','type_name','created_at','updated_at'].includes(sort)) return object[sort];
    const field=object.fields?.find(item=>item.id===sort&&!item.archived);
    if (!field||object.formula_errors?.[sort])return null;
    const raw=object.values?.[sort];
    return field.field_type==='progress'?(raw?.max>0?raw.current/raw.max:null):raw;
  };
  return result.sort((a,b)=>{
    const x=value(a),y=value(b),xm=missing(x),ym=missing(y);
    if(xm!==ym)return xm?1:-1; // Missing values always last, in either direction.
    let order=0;
    if(!xm)order=typeof x==='number'&&typeof y==='number'?x-y:typeof x==='boolean'&&typeof y==='boolean'?Number(x)-Number(y):String(x).localeCompare(String(y),undefined,{numeric:true,sensitivity:'base'});
    return order*direction || a.id.localeCompare(b.id);
  });
}
