"""Tommy with a Catmull-Clark subdivision on every mesh: models_obj.py's tommy(),
loaded without its main(), with write_obj wrapped to add the modifier first."""
import os, re, sys, types
# AmoledOS's checkout next to this one, or wherever $AMOLEDOS says
TOOLS = os.path.join(os.environ.get('AMOLEDOS', os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../ESP32S3_AmoledOS')),
                     'apps/visor3d/tools')
argv = sys.argv[sys.argv.index('--') + 1:]
out, levels, which = argv[0], int(argv[1]), argv[2] if len(argv) > 2 else 'tommy'
src = open(os.path.join(TOOLS, 'models_obj.py')).read()
src = re.sub(r'\nmain\(\)\s*$', '\n', src)
mod = types.ModuleType('models_obj'); mod.__file__ = os.path.join(TOOLS, 'models_obj.py')
exec(compile(src, mod.__file__, 'exec'), mod.__dict__)
orig = mod.write_obj
def write_obj(out_, stem, title, objs, colour, **kw):
    for ob in objs:
        if ob.type == 'MESH':
            m = ob.modifiers.new('hd', 'SUBSURF'); m.levels = levels; m.render_levels = levels
            # the face and the emblem are thin shells laid on the head and the
            # shirt: smoothing shrinks them into what they sit on, so they
            # are only cut, in place
            if ob.name in ('t_face', 't_emblem'):
                m.subdivision_type = 'SIMPLE'
    return orig(out_, stem, title + ' (HD, subsurf %d)' % levels, objs, colour, **kw)
mod.write_obj = write_obj
mod.__dict__[which](out)
