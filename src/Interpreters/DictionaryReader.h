#pragma once

#include <Common/typeid_cast.h>
#include <Core/Block.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Columns/ColumnVector.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnConst.h>
#include <Interpreters/Context.h>
#include <Functions/FunctionFactory.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_COLUMNS_DOESNT_MATCH;
    extern const int TYPE_MISMATCH;
}

class DictionaryReader
{
public:
    struct FunctionWrapper
    {
        ExecutableFunctionPtr function;
        ColumnNumbers arg_positions;
        size_t result_pos = 0;

        FunctionWrapper(const IFunctionOverloadResolver & resolver, const ColumnsWithTypeAndName & arguments, Block & block,
                        const ColumnNumbers & arg_positions_, const String & column_name, TypeIndex expected_type)
            : arg_positions(arg_positions_)
        {
            FunctionBasePtr prepare_function = resolver.build(arguments);
            result_pos = block.columns();

            ColumnWithTypeAndName result;
            result.name = "get_" + column_name;
            result.type = prepare_function->getReturnType();
            if (result.type->getTypeId() != expected_type)
                throw Exception("Type mismatch in dictionary reader for: " + column_name, ErrorCodes::TYPE_MISMATCH);

            function = prepare_function->prepare(block, arg_positions, result_pos);
        }

        void execute(Block & block, size_t rows) const
        {
            function->execute(block, arg_positions, result_pos, rows, false);
        }
    };

    DictionaryReader(const String & dictionary_name, const Names & src_column_names, const NamesAndTypesList & result_columns,
                     const Context & context, size_t key_size = 1)
        : result_header(makeResultBlock(result_columns))
        , key_position(key_size + result_header.columns())
    {
        if (src_column_names.size() != result_columns.size())
            throw Exception("Columns number mismatch in dictionary reader", ErrorCodes::NUMBER_OF_COLUMNS_DOESNT_MATCH);

        FunctionOverloadResolverPtr dict_has(FunctionFactory::instance().get("dictHas", context));
        FunctionOverloadResolverPtr dict_get(FunctionFactory::instance().get("dictGet", context));

        ColumnWithTypeAndName dict_name;
        ColumnWithTypeAndName key;
        ColumnWithTypeAndName column_name;

        {
            dict_name.name = "dict";
            dict_name.type = std::make_shared<DataTypeString>();
            dict_name.column = dict_name.type->createColumnConst(1, dictionary_name);

            /// TODO: composite key (key_size > 1)
            key.name = "key";
            key.type = std::make_shared<DataTypeUInt64>();

            column_name.name = "column";
            column_name.type = std::make_shared<DataTypeString>();
        }

        /// dictHas('dict_name', id)
        ColumnsWithTypeAndName arguments_has;
        arguments_has.push_back(dict_name);
        arguments_has.push_back(key);

        /// dictGet('dict_name', 'attr_name', id)
        ColumnsWithTypeAndName arguments_get;
        arguments_get.push_back(dict_name);
        arguments_get.push_back(column_name);
        arguments_get.push_back(key);

        sample_block.insert(dict_name);

        for (auto & columns_name : src_column_names)
        {
            ColumnWithTypeAndName name;
            name.name = "col_" + columns_name;
            name.type = std::make_shared<DataTypeString>();
            name.column = name.type->createColumnConst(1, columns_name);

            sample_block.insert(name);
        }

        sample_block.insert(key);

        ColumnNumbers positions_has{0, key_position};
        function_has = std::make_unique<FunctionWrapper>(
            *dict_has, arguments_has, sample_block, positions_has, "has", DataTypeUInt8().getTypeId());
        functions_get.reserve(result_header.columns());

        for (size_t i = 0; i < result_header.columns(); ++i)
        {
            size_t column_name_pos = key_size + i;
            auto & column = result_header.getByPosition(i);
            ColumnNumbers positions_get{0, column_name_pos, key_position};
            functions_get.emplace_back(FunctionWrapper(
                *dict_get, arguments_get, sample_block, positions_get, column.name, column.type->getTypeId()));
        }
    }

    void readKeys(const IColumn & keys, size_t size, Block & out_block, ColumnVector<UInt8>::Container & found,
                  std::vector<size_t> & positions) const
    {
        Block working_block = sample_block;
        size_t has_position = key_position + 1;

        /// set keys for dictHas()
        ColumnWithTypeAndName & key_column = working_block.getByPosition(key_position);
        key_column.column = keys.cloneResized(size); /// just a copy we cannot avoid

        /// calculate and extract dictHas()
        function_has->execute(working_block, size);
        ColumnWithTypeAndName & has_column = working_block.getByPosition(has_position);
        auto mutable_has = (*std::move(has_column.column)).mutate();
        found.swap(typeid_cast<ColumnVector<UInt8> &>(*mutable_has).getData());
        has_column.column = nullptr;

        /// set mapping form source keys to resulting rows in output block
        positions.clear();
        positions.resize(size, 0);
        size_t pos = 0;
        for (size_t i = 0; i < size; ++i)
            if (found[i])
                positions[i] = pos++;

        /// set keys for dictGet(): remove not found keys
        key_column.column = key_column.column->filter(found, -1);
        size_t rows = key_column.column->size();

        /// calculate dictGet()
        for (auto & func : functions_get)
            func.execute(working_block, rows);

        /// make result: copy header block with correct names and move data columns
        out_block = result_header.cloneEmpty();
        size_t first_get_position = has_position + 1;
        for (size_t i = 0; i < out_block.columns(); ++i)
        {
            auto & src_column = working_block.getByPosition(first_get_position + i);
            auto & dst_column = out_block.getByPosition(i);
            dst_column.column = src_column.column;
            src_column.column = nullptr;
        }
    }

private:
    Block result_header;
    Block sample_block; /// dictionary name, column names, key, dictHas() result, dictGet() results
    size_t key_position;
    std::unique_ptr<FunctionWrapper> function_has;
    std::vector<FunctionWrapper> functions_get;

    static Block makeResultBlock(const NamesAndTypesList & names)
    {
        Block block;
        for (auto & nm : names)
        {
            ColumnWithTypeAndName column{nullptr, nm.type, nm.name};
            if (column.type->isNullable())
                column.type = typeid_cast<const DataTypeNullable &>(*column.type).getNestedType();
            block.insert(std::move(column));
        }
        return block;
    }
};

}
