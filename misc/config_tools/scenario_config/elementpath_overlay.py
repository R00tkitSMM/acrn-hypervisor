#!/usr/bin/env python3
#
# Copyright (C), 2022 Intel Corporation.
# Copyright (c), 2018-2021, SISSA (International School for Advanced Studies).
#
# SPDX-License-Identifier: BSD-3-Clause
#

from decimal import Decimal
from copy import copy
import operator
import elementpath

BaseParser = elementpath.XPath2Parser

class CustomParser(BaseParser):
    SYMBOLS = BaseParser.SYMBOLS | {
        # Bit-wise operations
        'bitwise-and',

        'has',
        'duplicate-values',
        }

method = CustomParser.method
function = CustomParser.function

###
# Custom functions

OPERATORS_MAP = {
    'bitwise-and': operator.and_
}

@method(function('bitwise-and', nargs=2))
def evaluate(self, context=None):
    def to_int(value):
        if isinstance(value, int):
            return value
        elif isinstance(value, (float, Decimal)):
            return int(value)
        elif isinstance(value, str) and value.startswith("0x"):
            return int(value, base=16)
        else:
            raise TypeError('invalid type {!r} for xs:{}'.format(type(value), cls.name))

    def aux(op):
        op1, op2 = self.get_operands(context)
        if op1 is not None and op2 is not None:
            try:
                return op(to_int(op1), to_int(op2))
            except ValueError as err:
                raise self.error('FORG0001', err) from None
            except TypeError as err:
                raise self.error('XPTY0004', err)

    return aux(OPERATORS_MAP[self.symbol])

@method(function('has', nargs=2))
def evaluate_has_function(self, context=None):
    arg2 = self.get_argument(context, index=1, cls=str)
    for item in self[0].select(context):
        value = self.data_value(item)
        if value == arg2:
            return True
    return False

@method(function('duplicate-values', nargs=1))
def select_duplicate_values_function(self, context=None):
    def duplicate_values():
        results = []
        reported = []
        for item in self[0].select(context):
            value = self.data_value(item)
            if context is not None:
                context.item = value

            if value in results:
                if value not in reported:
                    yield value
                    reported.append(value)
            else:
                results.append(value)

    yield from duplicate_values()

###
# Collection of counter examples

class Hashable:
    def __init__(self, obj):
        self.obj = obj

    def __hash__(self):
        return id(self.obj)

def copy_context(context):
    ret = copy(context)
    if hasattr(context, 'counter_example'):
        ret.counter_example = dict()
    return ret

def add_counter_example(context, private_context, kvlist):
    if hasattr(context, 'counter_example'):
        context.counter_example.update(kvlist)
        if private_context:
            context.counter_example.update(private_context.counter_example)

@method('every')
@method('some')
def evaluate(self, context=None):
    if context is None:
        raise self.missing_context()

    some = self.symbol == 'some'
    varrefs = [Hashable(self[k]) for k in range(0, len(self) - 1, 2)]
    varnames = [self[k][0].value for k in range(0, len(self) - 1, 2)]
    selectors = [self[k].select for k in range(1, len(self) - 1, 2)]

    for results in copy(context).iter_product(selectors, varnames):
        private_context = copy_context(context)
        private_context.variables.update(x for x in zip(varnames, results))
        if self.boolean_value([x for x in self[-1].select(private_context)]):
            if some:
                add_counter_example(context, private_context, zip(varrefs, results))
                return True
        elif not some:
            add_counter_example(context, private_context, zip(varrefs, results))
            return False

    return not some

elementpath.XPath2Parser = CustomParser
