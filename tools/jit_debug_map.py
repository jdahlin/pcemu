"""Resolve timestamped JIT sidecars without confusing recycled host addresses."""
from bisect import bisect_right
import json


class TimedCodeMap:
    def __init__(self, path):
        self.epochs = []
        self.pid = None
        previous_time = -1
        with path.open() as stream:
            for line in stream:
                event = json.loads(line)
                if event['event'] == 'header':
                    if self.pid is not None or event['version'] not in (1, 2) or event['clock'] != 'unix-ns':
                        raise ValueError('Invalid JIT map header')
                    self.pid = event['pid']
                    self.version = event['version']
                    continue
                timestamp = event['timeNs']
                if self.pid is None or timestamp < previous_time:
                    raise ValueError('JIT map clock moved backwards or header missing')
                previous_time = timestamp
                if event['event'] == 'reset':
                    if self.epochs:
                        self.epochs[-1]['end'] = timestamp
                    self.epochs.append({'start': timestamp, 'end': None,
                                        'generation': event['generation'], 'blocks': [], 'addresses': []})
                elif event['event'] == 'load':
                    if not self.epochs or event['generation'] != self.epochs[-1]['generation']:
                        raise ValueError('JIT load has no matching arena generation')
                    size = event['end'] - event['start']
                    offsets = [r['offset'] for r in event['regions']]
                    if (size <= 0 or len(bytes.fromhex(event.pop('arm64'))) != size or
                            not offsets or offsets[0] != 0 or offsets != sorted(offsets) or
                            offsets[-1] >= size):
                        raise ValueError('Invalid JIT code or region range')
                    event['offsets'] = offsets
                    epoch = self.epochs[-1]
                    index = bisect_right(epoch['addresses'], event['start'])
                    if ((index and epoch['blocks'][index-1]['end'] > event['start']) or
                            (index < len(epoch['blocks']) and event['end'] > epoch['blocks'][index]['start'])):
                        raise ValueError('Overlapping JIT blocks within an arena generation')
                    epoch['blocks'].insert(index, event)
                    epoch['addresses'].insert(index, event['start'])
                elif event['event'] == 'patch':
                    if self.version < 2 or not self.epochs or event['generation'] != self.epochs[-1]['generation']:
                        raise ValueError('JIT patch has no matching arena generation')
                    epoch = self.epochs[-1]
                    index = bisect_right(epoch['addresses'], event['start']) - 1
                    if (index < 0 or event['start'] % 4 or event['end'] != event['start'] + 4 or
                            len(bytes.fromhex(event['arm64'])) != 4 or
                            event['end'] > epoch['blocks'][index]['end']):
                        raise ValueError('Invalid JIT patch range')
                else:
                    raise ValueError('Unknown JIT map event')
        if self.pid is None:
            raise ValueError('Empty JIT map')
        for epoch in self.epochs:
            epoch['blocks'].sort(key=lambda b: b['start'])
            epoch['addresses'] = [b['start'] for b in epoch['blocks']]
            for a, b in zip(epoch['blocks'], epoch['blocks'][1:]):
                if a['end'] > b['start']:
                    raise ValueError('Overlapping JIT blocks within an arena generation')
        self.times = [e['start'] for e in self.epochs]

    def lookup(self, address, timestamp, uncertainty_ns=1_000_000):
        # xctrace's exported recording start date has millisecond precision.
        # Reject samples close to a publication/recycle boundary conservatively.
        index = bisect_right(self.times, timestamp) - 1
        if index < 0:
            return None
        epoch = self.epochs[index]
        if epoch['end'] is not None and timestamp + uncertainty_ns >= epoch['end']:
            return None
        index = bisect_right(epoch['addresses'], address) - 1
        if index < 0:
            return None
        block = epoch['blocks'][index]
        if address >= block['end'] or timestamp - uncertainty_ns < block['timeNs']:
            return None
        region = bisect_right(block['offsets'], address - block['start']) - 1
        return block, block['regions'][region]
