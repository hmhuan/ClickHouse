#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/Transforms/ExpressionTransform.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Processors/Transforms/JoiningTransform.h>
#include <Interpreters/ExpressionActions.h>
#include <IO/Operators.h>
#include <Interpreters/JoinSwitcher.h>
#include <Common/JSONBuilder.h>

namespace DB
{

static ITransformingStep::Traits getTraits(const ActionsDAGPtr & actions, const Block & header, const SortDescription & sort_description)
{
    return ITransformingStep::Traits
    {
        {
            .preserves_distinct_columns = !actions->hasArrayJoin(),
            .returns_single_stream = false,
            .preserves_number_of_streams = true,
            .preserves_sorting = actions->isSortingPreserved(header, sort_description),
        },
        {
            .preserves_number_of_rows = !actions->hasArrayJoin(),
        }
    };
}

ExpressionStep::ExpressionStep(const DataStream & input_stream_, const ActionsDAGPtr & actions_dag_)
    : ITransformingStep(
        input_stream_,
        ExpressionTransform::transformHeader(input_stream_.header, *actions_dag_),
        getTraits(actions_dag_, input_stream_.header, input_stream_.sort_description))
    , actions_dag(actions_dag_)
{
    /// Some columns may be removed by expression.
    updateDistinctColumns(output_stream->header, output_stream->distinct_columns);
}

void ExpressionStep::transformPipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings & settings)
{
    auto expression = std::make_shared<ExpressionActions>(actions_dag, settings.getActionsSettings());

    pipeline.addSimpleTransform([&](const Block & header)
    {
        return std::make_shared<ExpressionTransform>(header, expression);
    });

    if (!blocksHaveEqualStructure(pipeline.getHeader(), output_stream->header))
    {
        auto convert_actions_dag = ActionsDAG::makeConvertingActions(
                pipeline.getHeader().getColumnsWithTypeAndName(),
                output_stream->header.getColumnsWithTypeAndName(),
                ActionsDAG::MatchColumnsMode::Name);
        auto convert_actions = std::make_shared<ExpressionActions>(convert_actions_dag, settings.getActionsSettings());

        pipeline.addSimpleTransform([&](const Block & header)
        {
            return std::make_shared<ExpressionTransform>(header, convert_actions);
        });
    }
}

void ExpressionStep::describeActions(FormatSettings & settings) const
{
    String prefix(settings.offset, ' ');
    bool first = true;

    auto expression = std::make_shared<ExpressionActions>(actions_dag);
    for (const auto & action : expression->getActions())
    {
        settings.out << prefix << (first ? "Actions: "
                                         : "         ");
        first = false;
        settings.out << action.toString() << '\n';
    }

    settings.out << prefix << "Positions:";
    for (const auto & pos : expression->getResultPositions())
        settings.out << ' ' << pos;
    settings.out << '\n';
}

void ExpressionStep::describeActions(JSONBuilder::JSONMap & map) const
{
    auto expression = std::make_shared<ExpressionActions>(actions_dag);
    map.add("Expression", expression->toTree());
}

namespace
{
    const ActionsDAG::Node * getOriginalNodeForOutputAlias(const ActionsDAGPtr & actions, const String & output_name)
    {
        /// find alias in output
        const ActionsDAG::Node * output_alias = nullptr;
        for (const auto * node : actions->getOutputs())
        {
            if (node->result_name == output_name)
            {
                output_alias = node;
                break;
            }
        }
        if (!output_alias)
        {
            // logDebug("getOriginalNodeForOutputAlias: no output alias found", output_name);
            return nullptr;
        }

        /// find original(non alias) node it refers to
        const ActionsDAG::Node * node = output_alias;
        while (node && node->type == ActionsDAG::ActionType::ALIAS)
        {
            chassert(!node->children.empty());
            node = node->children.front();
        }
        if (node && node->type != ActionsDAG::ActionType::INPUT)
            return nullptr;

        return node;
    }

}

void ExpressionStep::updateOutputStream()
{
    output_stream = createOutputStream(
        input_streams.front(), ExpressionTransform::transformHeader(input_streams.front().header, *actions_dag), getDataStreamTraits());

    const ActionsDAGPtr & actions = actions_dag;
    LOG_DEBUG(&Poco::Logger::get(__PRETTY_FUNCTION__), "ActionsDAG dump:\n{}", actions->dumpDAG());

    const auto & input_sort_description = getInputStreams().front().sort_description;
    for (size_t i = 0, s = input_sort_description.size(); i < s; ++i)
    {
        const auto & desc = input_sort_description[i];
        String alias;
        const auto & origin_column = desc.column_name;
        for (const auto & column : output_stream->header)
        {
            const auto * original_node = getOriginalNodeForOutputAlias(actions, column.name);
            if (original_node && original_node->result_name == origin_column)
            {
                alias = column.name;
                break;
            }
        }

        if (alias.empty())
            return;

        output_stream->sort_description[i].column_name = alias;
    }
}

}
