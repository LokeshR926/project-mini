#    Copyright 2023 The ChampSim Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import itertools
import collections
import os
import math

from . import defaults
from . import modules
from . import util

default_root = { 'block_size': 64, 'page_size': 4096, 'heartbeat_frequency': 10000000, 'num_cores': 1 }
default_core = { 'frequency' : 4000 }
default_pmem = { 'name': 'DRAM', 'frequency': 3200, 'channels': 1, 'ranks': 1, 'banks': 8, 'rows': 65536, 'columns': 128, 'lines_per_column': 8, 'channel_width': 8, 'wq_size': 64, 'rq_size': 64, 'tRP': 12.5, 'tRCD': 12.5, 'tCAS': 12.5, 'turn_around_time': 7.5 }
default_vmem = { 'pte_page_size': (1 << 12), 'num_levels': 5, 'minor_fault_penalty': 200 }

# Assign defaults that are unique per core
def upper_levels_for(system, name, key='lower_level'):
    finder = lambda x: x.get(key, '')
    upper_levels = sorted(system, key=finder)
    upper_levels = itertools.groupby(upper_levels, key=finder)
    return next(filter(lambda kv: kv[0] == name, upper_levels))[1]

# Scale frequencies
def scale_frequencies(it):
    it_a, it_b = itertools.tee(it, 2)
    max_freq = max(x['frequency'] for x in it_a)
    for x in it_b:
        x['frequency'] = max_freq / x['frequency']

def executable_name(*config_list):
    name_by_parts = '_'.join(('champsim', *(c.get('name') for c in config_list if c.get('name') is not None)))
    name_by_specification = next(reversed(list(c.get('executable_name') for c in config_list if c.get('executable_name') is not None)), name_by_parts)
    return name_by_specification

def duplicate_to_length(elements, n):
    repeat_factor = math.ceil(n / len(elements));
    return list(itertools.islice(itertools.chain(*(itertools.repeat(e, repeat_factor) for e in elements)), n))

def filter_inaccessible(system, roots, key='lower_level'):
    return util.combine_named(*(util.iter_system(system, r, key=key) for r in roots))

def module_parse(mod, context):
    if isinstance(mod, dict):
        return util.chain(util.subdict(mod, ('class',)), context.find(mod['path']))
    return context.find(mod)

def parse_config_in_context(merged_configs, branch_context, btb_context, prefetcher_context, replacement_context, compile_all_modules):
    config_file = util.chain(merged_configs, default_root)

    pmem = util.chain(config_file.get('physical_memory', {}), default_pmem)
    vmem = util.chain(config_file.get('virtual_memory', {}), default_vmem)

    # Copy or trim cores as necessary to fill out the specified number of cores
    cores = duplicate_to_length(config_file.get('ooo_cpu', [{}]), config_file['num_cores'])

    # Default core elements
    core_keys_to_copy = ('frequency', 'ifetch_buffer_size', 'decode_buffer_size', 'dispatch_buffer_size', 'rob_size', 'lq_size', 'sq_size', 'fetch_width', 'decode_width', 'dispatch_width', 'execute_width', 'lq_width', 'sq_width', 'retire_width', 'mispredict_penalty', 'scheduler_size', 'decode_latency', 'dispatch_latency', 'schedule_latency', 'execute_latency', 'branch_predictor', 'btb', 'DIB')
    cores = [util.chain(cpu, {'name': 'cpu'+str(i), 'index': i}, util.subdict(config_file, core_keys_to_copy), {'DIB': dict()}, default_core) for i,cpu in enumerate(cores)]

    pinned_cache_names = ('L1I', 'L1D', 'ITLB', 'DTLB', 'L2C', 'STLB')
    caches = util.combine_named(
            config_file.get('caches', []),

            # Copy values from the core specification, if these are dicts
            ({'name': util.read_element_name(core,name), **core[name]} for core,name in itertools.product(cores, pinned_cache_names) if isinstance(core.get(name), dict)),

            # Copy values from the config root, if these are dicts
            ({'name': util.read_element_name(core,name), **config_file[name]} for core,name in itertools.product(cores, pinned_cache_names) if isinstance(config_file.get(name), dict)),
            ({'name': 'LLC', **config_file.get('LLC', {})},),

            # Apply defaults named after the cores
            (defaults.core_defaults(cpu, 'L1I', ll_name='L2C', lt_name='ITLB') for cpu in cores),
            (defaults.core_defaults(cpu, 'L1D', ll_name='L2C', lt_name='DTLB') for cpu in cores),
            (defaults.core_defaults(cpu, 'ITLB', ll_name='STLB') for cpu in cores),
            (defaults.core_defaults(cpu, 'DTLB', ll_name='STLB') for cpu in cores),
            ({**defaults.core_defaults(cpu, 'L2C', lt_name='STLB'), 'lower_level': 'LLC'} for cpu in cores),
            (defaults.core_defaults(cpu, 'STLB', ll_name='PTW') for cpu in cores)
            )

    ptws = util.combine_named(
            config_file.get('ptws',[]),

            # Copy values from the core specification, if these are dicts
            ({'name': util.read_element_name(c,'PTW'), **c['PTW']} for c in cores if isinstance(c.get('PTW'), dict)),

            # Copy values from the config root, if these are dicts
            ({'name': util.read_element_name(c,'PTW'), **config_file['PTW']} for c in cores if isinstance(config_file.get('PTW'), dict)),

            # Apply defaults named after the cores
            (defaults.core_defaults(cpu, 'PTW', ll_name='L1D') for cpu in cores)
            )

    # Convert all core values to labels
    cores = [util.chain({n: util.read_element_name(cpu, n) for n in (*pinned_cache_names, 'PTW')}, cpu) for cpu in cores]

    # The name 'DRAM' is reserved for the physical memory
    caches = {k:v for k,v in caches.items() if k != 'DRAM'}

    # Frequencies are the maximum of the upper levels, unless specified
    for cpu,name in itertools.product(cores, ('L1I', 'L1D', 'ITLB', 'DTLB')):
        caches = util.combine_named(caches.values(),
            itertools.islice(itertools.accumulate(
                itertools.chain((cpu,), util.iter_system(caches, cpu[name])),
                lambda u,l: {'name': l['name'], 'frequency': max(u.get('frequency', 0), l.get('frequency', 0))}
            ), 1, None)
        )

    # Inferred defaults for the first three levels down the instruction path
    l1i_path_defaults = (
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(cores, name, key='L1I'), set_factor=64, mshr_factor=32, bandwidth_factor=1),
            '_first_level': True, '_is_instruction_cache': True,
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_l1i'
        }),
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(caches.values(), name), set_factor=512, mshr_factor=32, bandwidth_factor=0.5),
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_l2c'
        }),
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(caches.values(), name), set_factor=2048, mshr_factor=64, bandwidth_factor=1),
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_llc'
        })
    )

    # Inferred defaults for the first three levels down the data path
    l1d_path_defaults = (
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(cores, name, key='L1D'), set_factor=64, mshr_factor=32, bandwidth_factor=1),
            '_first_level': True,
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_l1d'
        }),
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(caches.values(), name), set_factor=512, mshr_factor=32, bandwidth_factor=0.5),
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_l2c'
        }),
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(caches.values(), name), set_factor=2048, mshr_factor=64, bandwidth_factor=1),
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_llc'
        })
    )

    # Inferred defaults for the first two levels down the instruction translation path
    itlb_path_defaults = (
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(cores, name, key='ITLB'), set_factor=16, queue_factor=16, mshr_factor=8, bandwidth_factor=1),
            '_first_level': True,
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_itlb'
        }),
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(caches.values(), name), set_factor=64, mshr_factor=8, bandwidth_factor=0.5),
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_stlb'
        })
    )

    # Inferred defaults for the first two levels down the data translation path
    dtlb_path_defaults = (
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(cores, name, key='DTLB'), set_factor=16, queue_factor=16, mshr_factor=8, bandwidth_factor=1),
            '_first_level': True,
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_dtlb'
        }),
        (lambda name: {
            'name': name,
            **defaults.ul_dependent_defaults(*upper_levels_for(caches.values(), name), set_factor=64, mshr_factor=8, bandwidth_factor=0.5),
            'prefetcher': 'no', 'replacement': 'lru',
            '_defaults': 'champsim::defaults::default_stlb'
        })
    )

    caches = util.combine_named(
            caches.values(),
            *(map(lambda f,c: f(c['name']), l1i_path_defaults, path) for path in [util.iter_system(caches, cpu['L1I']) for cpu in cores]),
            *(map(lambda f,c: f(c['name']), l1d_path_defaults, path) for path in [util.iter_system(caches, cpu['L1D']) for cpu in cores]),
            *(map(lambda f,c: f(c['name']), itlb_path_defaults, path) for path in [util.iter_system(caches, cpu['ITLB']) for cpu in cores]),
            *(map(lambda f,c: f(c['name']), dtlb_path_defaults, path) for path in [util.iter_system(caches, cpu['DTLB']) for cpu in cores])
            )

    # Apply defaults to PTW
    ptws = util.combine_named(
            ptws.values(),
            ({
                'name': cpu['PTW'],
                **defaults.ul_dependent_defaults(*upper_levels_for(caches.values(), cpu['PTW']), queue_factor=16, mshr_factor=5, bandwidth_factor=2),
                'frequency': cpu['frequency'],
                'cpu': cpu['index']
            } for cpu in cores)
            )

    ## DEPRECATION
    # The keys "max_read" and "max_write" are deprecated. For now, permit them but print a warning
    for cache in caches.values():
        if "max_read" in cache:
            print('WARNING: key "max_read" in cache ', cache['name'], ' is deprecated. Use "max_tag_check" instead.')
            cache['max_tag_check'] = cache['max_read']
        if "max_write" in cache:
            print('WARNING: key "max_write" in cache ', cache['name'], ' is deprecated. Use "max_fill" instead.')
            cache['max_fill'] = cache['max_write']

    # Remove caches that are inaccessible
    caches = filter_inaccessible(caches, [cpu[name] for cpu,name in itertools.product(cores, ('ITLB', 'DTLB', 'L1I', 'L1D'))])

    pmem['io_freq'] = pmem['frequency'] # Save value
    scale_frequencies(itertools.chain(cores, caches.values(), ptws.values(), (pmem,)))

    # TODO can these be removed in favor of the defaults in inc/defaults.hpp?
    # All cores have a default branch predictor and BTB
    for c in cores:
        c.setdefault('branch_predictor', 'bimodal')
        c.setdefault('btb', 'basic_btb')

    # Mark queues that need to match full addresses on collision
    caches = util.combine_named(caches.values(), ({'name': k, '_queue_check_full_addr': c.get('_first_level', False) or c.get('wq_check_full_addr', False)} for k,c in caches.items()))

    # TLBs use page offsets, Caches use block offsets
    tlb_path = itertools.chain.from_iterable(util.iter_system(caches, cpu[name]) for cpu,name in itertools.product(cores, ('ITLB', 'DTLB')))
    l1d_path = itertools.chain.from_iterable(util.iter_system(caches, cpu[name]) for cpu,name in itertools.product(cores, ('L1I', 'L1D')))
    caches = util.combine_named(
            ({'name': c['name'], '_offset_bits': 'champsim::lg2(' + str(config_file['page_size']) + ')', '_needs_translate': False} for c in tlb_path),
            ({'name': c['name'], '_offset_bits': 'champsim::lg2(' + str(config_file['block_size']) + ')', '_needs_translate': c.get('_first_level', False) or c.get('virtual_prefetch', False)} for c in l1d_path),
            caches.values()
            )

    # The end of the data path is the physical memory
    caches = util.combine_named(
            caches.values(),
            ({'name': collections.deque(util.iter_system(caches, cpu['L1I']), maxlen=1)[0]['name'], 'lower_level': 'DRAM'} for cpu in cores),
            ({'name': collections.deque(util.iter_system(caches, cpu['L1D']), maxlen=1)[0]['name'], 'lower_level': 'DRAM'} for cpu in cores),
            )

    # Get module path names and unique module names
    caches = util.combine_named(caches.values(),
            ({'name': c['name'], '_replacement_data': [module_parse(m, replacement_context) for m in util.wrap_list(c.get('replacement',[]))]} for c in caches.values()),
            ({'name': c['name'], '_prefetcher_data': [util.chain({'_is_instruction_prefetcher': True} if c.get('_is_instruction_cache') else {}, module_parse(m, prefetcher_context)) for m in util.wrap_list(c.get('prefetcher',[]))]} for c in caches.values())
            )

    cores = list(util.combine_named(cores,
            ({'name': c['name'], '_branch_predictor_data': [module_parse(m, branch_context) for m in util.wrap_list(c.get('branch_predictor',[]))]} for c in cores),
            ({'name': c['name'], '_btb_data': [module_parse(m, btb_context) for m in util.wrap_list(c.get('btb',[]))]} for c in cores)
            ).values())

    elements = {'cores': cores, 'caches': tuple(caches.values()), 'ptws': tuple(ptws.values()), 'pmem': pmem, 'vmem': vmem}
    module_info = {
            'repl': util.combine_named(*(c['_replacement_data'] for c in caches.values()), replacement_context.find_all()),
            'pref': util.combine_named(*(c['_prefetcher_data'] for c in caches.values()), prefetcher_context.find_all()),
            'branch': util.combine_named(*(c['_branch_predictor_data'] for c in cores), branch_context.find_all()),
            'btb': util.combine_named(*(c['_btb_data'] for c in cores), btb_context.find_all())
            }

    if compile_all_modules:
        modules_to_compile = [*set(itertools.chain(*(d.keys() for d in module_info.values())))]
    else:
        modules_to_compile = [*set(d['name'] for d in itertools.chain(
            *(c['_replacement_data'] for c in caches.values()),
            *(c['_prefetcher_data'] for c in caches.values()),
            *(c['_branch_predictor_data'] for c in cores),
            *(c['_btb_data'] for c in cores)
        ))]

    env_vars = ('CC', 'CXX', 'CPPFLAGS', 'CXXFLAGS', 'LDFLAGS', 'LDLIBS')
    extern_config_file_keys = ('block_size', 'page_size', 'heartbeat_frequency', 'num_cores')

    return elements, modules_to_compile, module_info, util.subdict(config_file, extern_config_file_keys), util.subdict(config_file, env_vars)

def parse_config(*configs, module_dir=[], branch_dir=[], btb_dir=[], pref_dir=[], repl_dir=[], compile_all_modules=False):
    champsim_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    name = executable_name(*configs)
    elements, modules_to_compile, module_info, config_file, env = parse_config_in_context(util.chain(*configs),
        branch_context = modules.ModuleSearchContext([*(os.path.join(m, 'branch') for m in module_dir), *branch_dir, os.path.join(champsim_root, 'branch')]),
        btb_context = modules.ModuleSearchContext([*(os.path.join(m, 'btb') for m in module_dir), *btb_dir, os.path.join(champsim_root, 'btb')]),
        replacement_context = modules.ModuleSearchContext([*(os.path.join(m, 'replacement') for m in module_dir), *repl_dir, os.path.join(champsim_root, 'replacement')]),
        prefetcher_context = modules.ModuleSearchContext([*(os.path.join(m, 'prefetcher') for m in module_dir), *pref_dir, os.path.join(champsim_root, 'prefetcher')]),
        compile_all_modules = compile_all_modules
    )

    module_info = {
            'repl': {k: util.chain(v, modules.get_repl_data(v['name'])) for k,v in module_info['repl'].items()},
            'pref': {k: util.chain(v, modules.get_pref_data(v['name'], v.get('_is_instruction_prefetcher',False))) for k,v in module_info['pref'].items()},
            'branch': {k: util.chain(v, modules.get_branch_data(v['name'])) for k,v in module_info['branch'].items()},
            'btb': {k: util.chain(v, modules.get_btb_data(v['name'])) for k,v in module_info['btb'].items()},
            }

    return name, elements, modules_to_compile, module_info, config_file, env

