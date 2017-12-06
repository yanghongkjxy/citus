/*-------------------------------------------------------------------------
 *
 * distributed_planner.c
 *	  General Citus planner code.
 *
 * Copyright (c) 2012-2016, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include <float.h>
#include <limits.h>

#include "catalog/pg_type.h"

#include "distributed/citus_nodefuncs.h"
#include "distributed/citus_nodes.h"
#include "distributed/citus_ruleutils.h"
#include "distributed/errormessage.h"
#include "distributed/insert_select_planner.h"
#include "distributed/metadata_cache.h"
#include "distributed/multi_executor.h"
#include "distributed/distributed_planner.h"
#include "distributed/multi_logical_optimizer.h"
#include "distributed/multi_logical_planner.h"
#include "distributed/multi_partitioning_utils.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/multi_master_planner.h"
#include "distributed/multi_router_planner.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/print.h"
#include "parser/parsetree.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


#define CURSOR_OPT_FORCE_DISTRIBUTED 0x080000


static List *plannerRestrictionContextList = NIL;
int MultiTaskQueryLogLevel = MULTI_TASK_QUERY_INFO_OFF; /* multi-task query log level */
static uint64 NextPlanId = 1;


/*
 * CteReferenceWalkerContext is used to collect CTE references in
 * CteReferenceListWalker.
 */
typedef struct CteReferenceWalkerContext
{
	int level;
	List *cteReferenceList;
} CteReferenceWalkerContext;

/*
 * VarLevelsUpWalkerContext is used to find Vars in a (sub)query that
 * refer to upper levels and therefore cannot be planned separately.
 */
typedef struct VarLevelsUpWalkerContext
{
	int level;
} VarLevelsUpWalkerContext;

/*
 * PlanPullPushContext is used to recursively plan subqueries
 * and CTEs, pull results to the coordinator, and push it back into
 * the workers.
 */
typedef struct PlanPullPushContext
{
	uint64 planId;
	PlannerRestrictionContext *plannerRestrictionContext;
	List *subPlanList;
	int level;
} PlanPullPushContext;


typedef struct QueryReplaceViaRTEIdentityContext
{
	PlanPullPushContext *pullPushContext;
	int rteIdentity;
} QueryReplaceViaRTEIdentityContext;

/* local function forward declarations */
static bool NeedsDistributedPlanningWalker(Node *node, void *context);
static PlannedStmt * CreateDistributedPlan(uint64 planId, PlannedStmt *localPlan,
										   Query *originalQuery, Query *query,
										   ParamListInfo boundParams,
										   PlannerRestrictionContext *
										   plannerRestrictionContext);
static DistributedPlan * CreateDistributedSelectPlan(uint64 planId, Query *originalQuery,
													 Query *query,
													 ParamListInfo boundParams,
													 bool hasUnresolvedParams,
													 PlannerRestrictionContext *
													 plannerRestrictionContext);
static void AssignRTEIdentities(Query *queryTree);
static void AssignRTEIdentity(RangeTblEntry *rangeTableEntry, int rteIdentifier);
static void AdjustPartitioningForDistributedPlanning(Query *parse,
													 bool setPartitionedTablesInherited);
static PlannedStmt * FinalizePlan(PlannedStmt *localPlan,
								  DistributedPlan *distributedPlan);
static PlannedStmt * FinalizeNonRouterPlan(PlannedStmt *localPlan,
										   DistributedPlan *distributedPlan,
										   CustomScan *customScan);
static DeferredErrorMessage * PlanPullPushCTEs(Query *query,
											   PlanPullPushContext *context);
static bool CteReferenceListWalker(Node *node, CteReferenceWalkerContext *context);
static bool ContainsResultFunctionWalker(Node *node, void *context);
static DeferredErrorMessage * PlanPullPushSubqueries(Query *query,
													 PlanPullPushContext *context);
static void RecursivelyPlanSetOperations(Query *query, Node *node,
										 PlanPullPushContext *context);
static void RecursivelyPlanRecurringOuterJoins(Query *query, Node *node,
											   PlanPullPushContext *context);
static bool QueryContainsDistributedTableRTE(Query *query);
static bool JoinTreeContainsDistributedTableRTE(Query *query, Node *node);
static bool IsDistributedTableRTE(Node *node);
static bool PlanPullPushSubqueriesWalker(Node *node, PlanPullPushContext *context);
static bool ShouldRecursivelyPlanSubquery(Query *query, PlanPullPushContext *context);
static PlannedStmt * RecursivelyPlanQuery(Query *query, uint64 planId, int subPlanId);
static bool ContainsReferencesToOuterQuery(Query *query);
static bool ContainsReferencesToOuterQueryWalker(Node *node,
												 VarLevelsUpWalkerContext *context);
static Query * BuildSubPlanResultQuery(Query *subquery, uint64 planId, int subPlanId);
static PlannedStmt * FinalizeRouterPlan(PlannedStmt *localPlan, CustomScan *customScan);
static void CheckNodeIsDumpable(Node *node);
static Node * CheckNodeCopyAndSerialization(Node *node);
static List * CopyPlanParamList(List *originalPlanParamList);
static PlannerRestrictionContext * CreateAndPushPlannerRestrictionContext(void);
static PlannerRestrictionContext * CurrentPlannerRestrictionContext(void);
static void PopPlannerRestrictionContext(void);
static bool HasUnresolvedExternParamsWalker(Node *expression, ParamListInfo boundParams);


/* functions for replacing non-colocated joins */
static bool ReplaceNonColocatedJoin(PlanPullPushContext *context, Query *query);
static bool ReplaceSubqueryViaRTEIdentity(Query *query,
										  PlanPullPushContext *pullPushContext,
										  int rteIdentity);
static bool ReplaceSubqueryViaRTEIdentityWalker(Node *node, void *context);


/* Distributed planner hook */
PlannedStmt *
distributed_planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result = NULL;
	bool needsDistributedPlanning = NeedsDistributedPlanning(parse);
	Query *originalQuery = NULL;
	PlannerRestrictionContext *plannerRestrictionContext = NULL;
	bool setPartitionedTablesInherited = false;

	if (cursorOptions & CURSOR_OPT_FORCE_DISTRIBUTED)
	{
		needsDistributedPlanning = true;
	}

	/*
	 * standard_planner scribbles on it's input, but for deparsing we need the
	 * unmodified form. Note that we keep RTE_RELATIONs with their identities
	 * set, which doesn't break our goals, but, prevents us keeping an extra copy
	 * of the query tree. Note that we copy the query tree once we're sure it's a
	 * distributed query.
	 */
	if (needsDistributedPlanning)
	{
		setPartitionedTablesInherited = false;

		AssignRTEIdentities(parse);
		originalQuery = copyObject(parse);

		AdjustPartitioningForDistributedPlanning(parse, setPartitionedTablesInherited);
	}

	/* create a restriction context and put it at the end if context list */
	plannerRestrictionContext = CreateAndPushPlannerRestrictionContext();

	PG_TRY();
	{
		/*
		 * First call into standard planner. This is required because the Citus
		 * planner relies on parse tree transformations made by postgres' planner.
		 */

		result = standard_planner(parse, cursorOptions, boundParams);

		if (needsDistributedPlanning)
		{
			uint64 planId = NextPlanId++;

			result = CreateDistributedPlan(planId, result, originalQuery, parse,
										   boundParams, plannerRestrictionContext);
		}
	}
	PG_CATCH();
	{
		PopPlannerRestrictionContext();
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (needsDistributedPlanning)
	{
		setPartitionedTablesInherited = true;

		AdjustPartitioningForDistributedPlanning(parse, setPartitionedTablesInherited);
	}

	/* remove the context from the context list */
	PopPlannerRestrictionContext();

	/*
	 * In some cases, for example; parameterized SQL functions, we may miss that
	 * there is a need for distributed planning. Such cases only become clear after
	 * standart_planner performs some modifications on parse tree. In such cases
	 * we will simply error out.
	 */
	if (!needsDistributedPlanning && NeedsDistributedPlanning(parse))
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot perform distributed planning on this "
							   "query because parameterized queries for SQL "
							   "functions referencing distributed tables are "
							   "not supported"),
						errhint("Consider using PL/pgSQL functions instead.")));
	}

	return result;
}


/*
 * NeedsDistributedPlanning checks if the passed in query is a query running
 * on a distributed table. If it is, we start distributed planning.
 */
bool
NeedsDistributedPlanning(Query *queryTree)
{
	CmdType commandType = queryTree->commandType;
	if (commandType != CMD_SELECT && commandType != CMD_INSERT &&
		commandType != CMD_UPDATE && commandType != CMD_DELETE)
	{
		return false;
	}

	if (!CitusHasBeenLoaded())
	{
		return false;
	}

	/*
	 * We can handle INSERT INTO distributed_table SELECT ... even if the SELECT
	 * part references local tables, so skip the remaining checks.
	 */
	if (InsertSelectIntoDistributedTable(queryTree))
	{
		return true;
	}

	if (!NeedsDistributedPlanningWalker((Node *) queryTree, NULL))
	{
		return false;
	}

	if (InsertSelectIntoLocalTable(queryTree))
	{
		ereport(ERROR, (errmsg("cannot INSERT rows from a distributed query into a "
							   "local table")));
	}

	return true;
}


static bool
NeedsDistributedPlanningWalker(Node *node, void *context)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;
		ListCell *rangeTableCell = NULL;
		bool hasLocalRelation = false;
		bool hasDistributedRelation = false;

		foreach(rangeTableCell, query->rtable)
		{
			RangeTblEntry *rangeTableEntry = (RangeTblEntry *) lfirst(rangeTableCell);
			Oid relationId = InvalidOid;

			if (rangeTableEntry->rtekind != RTE_RELATION ||
				rangeTableEntry->relkind == RELKIND_VIEW)
			{
				/* only consider tables */
				continue;
			}

			relationId = rangeTableEntry->relid;
			if (IsDistributedTable(relationId))
			{
				hasDistributedRelation = true;
			}
			else
			{
				hasLocalRelation = true;
			}
		}

		if (hasLocalRelation && hasDistributedRelation)
		{
			ereport(ERROR, (errmsg("cannot plan queries which join local and "
								   "distributed relations")));
		}

		if (hasDistributedRelation)
		{
			return true;
		}

		return query_tree_walker(query, NeedsDistributedPlanningWalker, NULL, 0);
	}
	else
	{
		return expression_tree_walker(node, NeedsDistributedPlanningWalker, NULL);
	}
}


/*
 * AssignRTEIdentities function modifies query tree by adding RTE identities to the
 * RTE_RELATIONs.
 *
 * Please note that, we want to avoid modifying query tree as much as possible
 * because if PostgreSQL changes the way it uses modified fields, that may break
 * our logic.
 */
static void
AssignRTEIdentities(Query *queryTree)
{
	List *rangeTableList = NIL;
	ListCell *rangeTableCell = NULL;
	int rteIdentifier = 1;

	/* extract range table entries for simple relations only */
	ExtractRangeTableEntryWalker((Node *) queryTree, &rangeTableList);

	foreach(rangeTableCell, rangeTableList)
	{
		RangeTblEntry *rangeTableEntry = (RangeTblEntry *) lfirst(rangeTableCell);

		/*
		 * To be able to track individual RTEs through PostgreSQL's query
		 * planning, we need to be able to figure out whether an RTE is
		 * actually a copy of another, rather than a different one. We
		 * simply number the RTEs starting from 1.
		 *
		 * Note that we're only interested in RTE_RELATIONs and thus assigning
		 * identifiers to those RTEs only.
		 */
		if (rangeTableEntry->rtekind == RTE_RELATION)
		{
			AssignRTEIdentity(rangeTableEntry, rteIdentifier++);
		}
	}
}


/*
 * AdjustPartitioningForDistributedPlanning function modifies query tree by
 * changing inh flag and relkind of partitioned tables. We want Postgres to
 * treat partitioned tables as regular relations (i.e. we do not want to
 * expand them to their partitions) since it breaks Citus planning in different
 * ways. We let anything related to partitioning happen on the shards.
 *
 * Please note that, we want to avoid modifying query tree as much as possible
 * because if PostgreSQL changes the way it uses modified fields, that may break
 * our logic.
 */
static void
AdjustPartitioningForDistributedPlanning(Query *queryTree,
										 bool setPartitionedTablesInherited)
{
	List *rangeTableList = NIL;
	ListCell *rangeTableCell = NULL;

	/* extract range table entries for simple relations only */
	ExtractRangeTableEntryWalker((Node *) queryTree, &rangeTableList);

	foreach(rangeTableCell, rangeTableList)
	{
		RangeTblEntry *rangeTableEntry = (RangeTblEntry *) lfirst(rangeTableCell);

		/*
		 * We want Postgres to behave partitioned tables as regular relations
		 * (i.e. we do not want to expand them to their partitions). To do this
		 * we set each distributed partitioned table's inh flag to appropriate
		 * value before and after dropping to the standart_planner.
		 */
		if (IsDistributedTable(rangeTableEntry->relid) &&
			PartitionedTable(rangeTableEntry->relid))
		{
			rangeTableEntry->inh = setPartitionedTablesInherited;

#if (PG_VERSION_NUM >= 100000)
			if (setPartitionedTablesInherited)
			{
				rangeTableEntry->relkind = RELKIND_PARTITIONED_TABLE;
			}
			else
			{
				rangeTableEntry->relkind = RELKIND_RELATION;
			}
#endif
		}
	}
}


/*
 * AssignRTEIdentity assigns the given rteIdentifier to the given range table
 * entry.
 *
 * To be able to track RTEs through postgres' query planning, which copies and
 * duplicate, and modifies them, we sometimes need to figure out whether two
 * RTEs are copies of the same original RTE. For that we, hackishly, use a
 * field normally unused in RTE_RELATION RTEs.
 *
 * The assigned identifier better be unique within a plantree.
 */
static void
AssignRTEIdentity(RangeTblEntry *rangeTableEntry, int rteIdentifier)
{
	Assert(rangeTableEntry->rtekind == RTE_RELATION);


	rangeTableEntry->values_lists = list_make1_int(rteIdentifier);
}


/* GetRTEIdentity returns the identity assigned with AssignRTEIdentity. */
int
GetRTEIdentity(RangeTblEntry *rte)
{
	Assert(rte->rtekind == RTE_RELATION);
	Assert(IsA(rte->values_lists, IntList));
	Assert(list_length(rte->values_lists) == 1);

	return linitial_int(rte->values_lists);
}


/*
 * IsModifyCommand returns true if the query performs modifications, false
 * otherwise.
 */
bool
IsModifyCommand(Query *query)
{
	CmdType commandType = query->commandType;

	if (commandType == CMD_INSERT || commandType == CMD_UPDATE ||
		commandType == CMD_DELETE)
	{
		return true;
	}

	return false;
}


/*
 * IsMultiShardModifyPlan returns true if the given plan was generated for
 * multi shard update or delete query.
 */
bool
IsMultiShardModifyPlan(DistributedPlan *distributedPlan)
{
	if (IsUpdateOrDelete(distributedPlan) && IsMultiTaskPlan(distributedPlan))
	{
		return true;
	}

	return false;
}


/*
 * IsMultiTaskPlan returns true if job contains multiple tasks.
 */
bool
IsMultiTaskPlan(DistributedPlan *distributedPlan)
{
	Job *workerJob = distributedPlan->workerJob;

	if (workerJob != NULL && list_length(workerJob->taskList) > 1)
	{
		return true;
	}

	return false;
}


/*
 * IsUpdateOrDelete returns true if the query performs update or delete.
 */
bool
IsUpdateOrDelete(DistributedPlan *distributedPlan)
{
	CmdType commandType = distributedPlan->operation;

	if (commandType == CMD_UPDATE || commandType == CMD_DELETE)
	{
		return true;
	}

	return false;
}


/*
 * IsModifyDistributedPlan returns true if the multi plan performs modifications,
 * false otherwise.
 */
bool
IsModifyDistributedPlan(DistributedPlan *distributedPlan)
{
	bool isModifyDistributedPlan = false;
	CmdType operation = distributedPlan->operation;

	if (operation == CMD_INSERT || operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		isModifyDistributedPlan = true;
	}

	return isModifyDistributedPlan;
}


/*
 * CreateDistributedPlan encapsulates the logic needed to transform a particular
 * query into a distributed plan.
 */
static PlannedStmt *
CreateDistributedPlan(uint64 planId, PlannedStmt *localPlan, Query *originalQuery,
					  Query *query, ParamListInfo boundParams,
					  PlannerRestrictionContext *plannerRestrictionContext)
{
	DistributedPlan *distributedPlan = NULL;
	PlannedStmt *resultPlan = NULL;
	bool hasUnresolvedParams = false;

	if (HasUnresolvedExternParamsWalker((Node *) originalQuery, boundParams))
	{
		hasUnresolvedParams = true;
	}

	if (IsModifyCommand(query))
	{
		EnsureModificationsCanRun();

		if (InsertSelectIntoDistributedTable(originalQuery))
		{
			distributedPlan =
				CreateInsertSelectPlan(originalQuery, plannerRestrictionContext);
		}
		else
		{
			/* modifications are always routed through the same planner/executor */
			distributedPlan =
				CreateModifyPlan(originalQuery, query, plannerRestrictionContext);
		}

		Assert(distributedPlan);
	}
	else
	{
		distributedPlan =
			CreateDistributedSelectPlan(planId, originalQuery, query, boundParams,
										hasUnresolvedParams,
										plannerRestrictionContext);
	}

	/*
	 * If no plan was generated, prepare a generic error to be emitted.
	 * Normally this error message will never returned to the user, as it's
	 * usually due to unresolved prepared statement parameters - in that case
	 * the logic below will force a custom plan (i.e. with parameters bound to
	 * specific values) to be generated.  But sql (not plpgsql) functions
	 * unfortunately don't go through a codepath supporting custom plans - so
	 * we still need to have an error prepared.
	 */
	if (!distributedPlan)
	{
		/* currently always should have a more specific error otherwise */
		Assert(hasUnresolvedParams);
		distributedPlan = CitusMakeNode(DistributedPlan);
		distributedPlan->planningError =
			DeferredError(ERRCODE_FEATURE_NOT_SUPPORTED,
						  "could not create distributed plan",
						  "Possibly this is caused by the use of parameters in SQL "
						  "functions, which is not supported in Citus.",
						  "Consider using PL/pgSQL functions instead.");
	}

	/*
	 * Error out if none of the planners resulted in a usable plan, unless the
	 * error was possibly triggered by missing parameters.  In that case we'll
	 * not error out here, but instead rely on postgres' custom plan logic.
	 * Postgres re-plans prepared statements the first five executions
	 * (i.e. it produces custom plans), after that the cost of a generic plan
	 * is compared with the average custom plan cost.  We support otherwise
	 * unsupported prepared statement parameters by assigning an exorbitant
	 * cost to the unsupported query.  That'll lead to the custom plan being
	 * chosen.  But for that to be possible we can't error out here, as
	 * otherwise that logic is never reached.
	 */
	if (distributedPlan->planningError && !hasUnresolvedParams)
	{
		RaiseDeferredError(distributedPlan->planningError, ERROR);
	}

	/* remember the plan's identifier for identifying subplans */
	distributedPlan->planId = planId;

	/* create final plan by combining local plan with distributed plan */
	resultPlan = FinalizePlan(localPlan, distributedPlan);

	/*
	 * As explained above, force planning costs to be unrealistically high if
	 * query planning failed (possibly) due to prepared statement parameters or
	 * if it is planned as a multi shard modify query.
	 */
	if ((distributedPlan->planningError || IsMultiShardModifyPlan(distributedPlan)) &&
		hasUnresolvedParams)
	{
		/*
		 * Arbitraryly high cost, but low enough that it can be added up
		 * without overflowing by choose_custom_plan().
		 */
		resultPlan->planTree->total_cost = FLT_MAX / 100000000;
	}

	return resultPlan;
}


/*
 *
 */
static DistributedPlan *
CreateDistributedSelectPlan(uint64 planId, Query *originalQuery, Query *query,
							ParamListInfo boundParams, bool hasUnresolvedParams,
							PlannerRestrictionContext *plannerRestrictionContext)
{
	PlanPullPushContext pullPushContext = {
		planId, plannerRestrictionContext, NIL, 0
	};
	DistributedPlan *distributedPlan = NULL;
	MultiTreeRoot *logicalPlan = NULL;
	DeferredErrorMessage *pullPushError = NULL;

	/*
	 * For select queries we, if router executor is enabled, first try to
	 * plan the query as a router query. If not supported, otherwise try
	 * the full blown plan/optimize/physical planing process needed to
	 * produce distributed query plans.
	 */
	if (EnableRouterExecution)
	{
		RelationRestrictionContext *relationRestrictionContext =
			plannerRestrictionContext->relationRestrictionContext;

		distributedPlan = CreateRouterPlan(originalQuery, query,
										   relationRestrictionContext);
		if (distributedPlan != NULL)
		{
			if (distributedPlan->planningError == NULL)
			{
				/* successfully created a router plan */
				return distributedPlan;
			}
			else
			{
				/*
				 * For debugging it's useful to display why query was not
				 * router plannable.
				 */
				RaiseDeferredError(distributedPlan->planningError, DEBUG1);
			}
		}
	}

	if (hasUnresolvedParams)
	{
		/* remaining planners do not support unresolved parameters */
		return NULL;
	}

	/*
	 * The logical planner does not know how to deal with subqueries
	 * that require a merge step (e.g. aggregates, limit). Plan these
	 * subqueries separately and replace them with a subquery that
	 * scans intermediate results.
	 */
	pullPushError = PlanPullPushSubqueries(originalQuery, &pullPushContext);
	if (pullPushError != NULL)
	{
		/* PlanPullPushSubqueries only produces irrecoverable errors at the moment */
		RaiseDeferredError(pullPushError, ERROR);
	}

	/*
	 * If subqueries are executed using pull-push then we need to replan
	 * the query to get the new planner restriction context (without
	 * relations that appear in pull-push subqueries) and to apply
	 * planner transformations.
	 */
	if (list_length(pullPushContext.subPlanList) > 0)
	{
		bool setPartitionedTablesInherited = false;
		Query *newQuery = copyObject(originalQuery);

		/* remove the pre-transformation planner restrictions context */
		PopPlannerRestrictionContext();

		/* create a fresh new planner context */
		plannerRestrictionContext = CreateAndPushPlannerRestrictionContext();

		/* run the planner again to rebuild the planner restriction context */
		AssignRTEIdentities(newQuery);
		AdjustPartitioningForDistributedPlanning(newQuery, setPartitionedTablesInherited);

		standard_planner(newQuery, 0, boundParams);

		/* overwrite the old transformed query with the new transformed query */
		memcpy(query, newQuery, sizeof(Query));

		/* recurse into CreateDistributedSelectPlan with subqueries/CTEs replaced */
		distributedPlan = CreateDistributedSelectPlan(planId, originalQuery, query, NULL,
													  false, plannerRestrictionContext);
		distributedPlan->planId = planId;
		distributedPlan->subPlanList = pullPushContext.subPlanList;

		return distributedPlan;
	}

	logicalPlan = MultiLogicalPlanCreate(originalQuery, query, plannerRestrictionContext,
										 boundParams);
	MultiLogicalPlanOptimize(logicalPlan);

	/*
	 * This check is here to make it likely that all node types used in
	 * Citus are dumpable. Explain can dump logical and physical plans
	 * using the extended outfuncs infrastructure, but it's infeasible to
	 * test most plans. MultiQueryContainerNode always serializes the
	 * physical plan, so there's no need to check that separately
	 */
	CheckNodeIsDumpable((Node *) logicalPlan);

	/* Create the physical plan */
	distributedPlan = CreatePhysicalDistributedPlan(logicalPlan,
													plannerRestrictionContext);

	/* distributed plan currently should always succeed or error out */
	Assert(distributedPlan && distributedPlan->planningError == NULL);

	return distributedPlan;
}


static DeferredErrorMessage *
PlanPullPushSubqueries(Query *query, PlanPullPushContext *context)
{
	DeferredErrorMessage *error = NULL;

	if (SubqueryPushdown)
	{
		/*
		 * When the subquery_pushdown flag is enabled we make some hacks
		 * to push down subqueries with LIMIT. Recursive planning would
		 * valiantly do the right thing and try to recursively plan the
		 * inner subqueries, but we don't really want it to because those
		 * subqueries might not be supported and would be much slower.
		 *
		 * Instead, we skip recursive planning altogether when
		 * subquery_pushdown is enabled.
		 */
		return false;
	}

	error = PlanPullPushCTEs(query, context);
	if (error != NULL)
	{
		return error;
	}

	/* descend into subqueries */
	query_tree_walker(query, PlanPullPushSubqueriesWalker, context, 0);


	if (query->setOperations != NULL)
	{
		SetOperationStmt *setOperations = (SetOperationStmt *) query->setOperations;

		PlannerRestrictionContext *filteredRestrictionContext =
			FilterPlannerRestrictionForQuery(context->plannerRestrictionContext, query);

		if (setOperations->op != SETOP_UNION ||
			context->level == 0 ||
			DeferErrorIfUnsupportedUnionQuery(query) != NULL ||
			!SafeToPushdownUnionSubquery(filteredRestrictionContext))
		{
			SetOperationStmt *setOperations = (SetOperationStmt *) query->setOperations;

			RecursivelyPlanSetOperations(query, (Node *) setOperations, context);
		}
	}
	else
	{
		/* We've moved the same logic to RecursivelyPlanSubqueries() as well */
		PlannerRestrictionContext *filteredPlannerRestriction =
			FilterPlannerRestrictionForQuery((*context).plannerRestrictionContext,
											 query);

		if (ContainsUnionSubquery(query) && !SafeToPushdownUnionSubquery(
				filteredPlannerRestriction))
		{
			/* let it be handled in the next call */
		}
		else
		{
			/* handle non-colocated joins */
			while (!ContainsUnionSubquery(query) &&
				   !RestrictionEquivalenceForPartitionKeys(filteredPlannerRestriction))
			{
				/* if couldn't replace any queries, do not continue */
				if (!ReplaceNonColocatedJoin(context, query))
				{
					break;
				}

				/*
				 * We've replaced one of the non-colocated joins, now update
				 * the fitered restrictions so that the replaced join doesn't
				 * appear in the restrictions.
				 */
				filteredPlannerRestriction =
					FilterPlannerRestrictionForQuery((*context).plannerRestrictionContext,
													 query);
			}
		}
	}

	{
		FromExpr *fromExpr = query->jointree;
		ListCell *joinTreeNodeCell = NULL;

		foreach(joinTreeNodeCell, fromExpr->fromlist)
		{
			Node *joinTreeNode = (Node *) lfirst(joinTreeNodeCell);
			RecursivelyPlanRecurringOuterJoins(query, joinTreeNode, context);
		}
	}

	return NULL;
}


/*
 * Pick the RTE_RELATION that has the less number of distribution key joins.
 *
 * Later, find the subquery that incules this RTE_RELATION, and plan that. If we
 * can find and replace a join, we'd return true. Else, return false.
 */
static bool
ReplaceNonColocatedJoin(PlanPullPushContext *context, Query *query)
{

	if (NeedsDistributedPlanning(query) &&
		!ContainsReferencesToOuterQuery(query))
	{
		PlannerRestrictionContext *filteredPlannerRestriction =
			FilterPlannerRestrictionForQuery(context->plannerRestrictionContext,
											 query);
		int rteMin = FindRTEIdentityWithLeastColocatedJoins(
				filteredPlannerRestriction);

		return ReplaceSubqueryViaRTEIdentity(query, context, rteMin);
	}

	return false;
}


static bool
ReplaceSubqueryViaRTEIdentity(Query *query, PlanPullPushContext *pullPushContext,
							  int rteIdentity)
{
	QueryReplaceViaRTEIdentityContext queryReplaceViaRTEIdentityContext = {
		pullPushContext, rteIdentity
	};

	/*TODO: fix this hack. We want to run the ReplaceSubqueryViaRTEIdentityWalker on the query itself as well */
	if (query_tree_walker(query, ReplaceSubqueryViaRTEIdentityWalker,
						  &queryReplaceViaRTEIdentityContext, 0))
	{
		return true;
	}

	return false;
}


/*
 * Walks over the given node to replace the query with the given RTE identity.
 */
static bool
ReplaceSubqueryViaRTEIdentityWalker(Node *node, void *context)
{
	QueryReplaceViaRTEIdentityContext *replaceContext =
		(QueryReplaceViaRTEIdentityContext *) context;
	PlanPullPushContext *pullPushContext = replaceContext->pullPushContext;
	int rteIdentity = replaceContext->rteIdentity;
	uint64 planId = pullPushContext->planId;

	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;
		Relids queryRteIdentities = QueryRteIdentities(query);

		if (bms_is_member(rteIdentity, queryRteIdentities))
		{
			int subPlanId = list_length(pullPushContext->subPlanList);
			PlannedStmt *subPlan = RecursivelyPlanQuery(query, planId, subPlanId);

			pullPushContext->subPlanList = lappend(pullPushContext->subPlanList,
												   subPlan);

			return true;
		}
	}

	return expression_tree_walker(node, ReplaceSubqueryViaRTEIdentityWalker, context);
}


static void
RecursivelyPlanSetOperations(Query *query, Node *node,
							 PlanPullPushContext *context)
{
	if (IsA(node, SetOperationStmt))
	{
		SetOperationStmt *setOperations = (SetOperationStmt *) node;

		RecursivelyPlanSetOperations(query, setOperations->larg, context);
		RecursivelyPlanSetOperations(query, setOperations->rarg, context);
	}
	else if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rangeTableRef = (RangeTblRef *) node;
		RangeTblEntry *rangeTableEntry = rt_fetch(rangeTableRef->rtindex,
												  query->rtable);

		if (rangeTableEntry->rtekind == RTE_SUBQUERY &&
			QueryContainsDistributedTableRTE(rangeTableEntry->subquery))
		{
			Query *subquery = rangeTableEntry->subquery;
			uint64 planId = context->planId;
			int subPlanId = list_length(context->subPlanList);
			PlannedStmt *subPlan = RecursivelyPlanQuery(subquery, planId, subPlanId);
			context->subPlanList = lappend(context->subPlanList, subPlan);
		}
	}
}


/*
 * RecursivelyPlanJoinTree recursively plans all subqueries that contain
 * a distributed table RTE in a join tree. This is used to recursively
 * plan all leafs on the inner side of an outer join when the outer side
 * does not contain any distributed tables.
 *
 * This function currently plans each leaf node individually. A smarter
 * approach would be to wrap part of the join tree in a new subquery and
 * plan that recursively.
 */
static void
RecursivelyPlanJoinTree(Query *query, Node *joinTreeNode,
						PlanPullPushContext *context)
{
	if (IsA(joinTreeNode, JoinExpr))
	{
		JoinExpr *joinExpr = (JoinExpr *) joinTreeNode;

		RecursivelyPlanJoinTree(query, joinExpr->rarg, context);
		RecursivelyPlanJoinTree(query, joinExpr->larg, context);
	}
	else if (IsA(joinTreeNode, RangeTblRef))
	{
		RangeTblRef *rangeTableRef = (RangeTblRef *) joinTreeNode;
		RangeTblEntry *rangeTableEntry = rt_fetch(rangeTableRef->rtindex,
												  query->rtable);

		if (rangeTableEntry->rtekind == RTE_SUBQUERY &&
			QueryContainsDistributedTableRTE(rangeTableEntry->subquery))
		{
			Query *subquery = rangeTableEntry->subquery;
			uint64 planId = context->planId;
			int subPlanId = list_length(context->subPlanList);
			PlannedStmt *subPlan = RecursivelyPlanQuery(subquery, planId, subPlanId);
			context->subPlanList = lappend(context->subPlanList, subPlan);
		}
	}
}


/*
 * RecursivelyPlanRecurringOuterJoins looks for outer joins in the join tree
 * and if the outer side of the outer join does not contain a distributed
 * table RTE (meaning the same set of tuples recurs when joining with a shard),
 * while the inner side does, then the inner side is planned recursively.
 */
static void
RecursivelyPlanRecurringOuterJoins(Query *query, Node *joinTreeNode,
								   PlanPullPushContext *context)
{
	JoinExpr *joinExpr = NULL;
	JoinType joinType = JOIN_INNER;
	bool leftRecurs = false;
	bool rightRecurs = false;

	if (!IsA(joinTreeNode, JoinExpr))
	{
		/* nothing to do at leaf nodes */
		return;
	}

	joinExpr = (JoinExpr *) joinTreeNode;
	leftRecurs = !JoinTreeContainsDistributedTableRTE(query, joinExpr->larg);
	rightRecurs = !JoinTreeContainsDistributedTableRTE(query, joinExpr->rarg);
	joinType = joinExpr->jointype;

	switch (joinType)
	{
		case JOIN_LEFT:
		{
			/* recurse into right side if only left side is recurring */
			if (leftRecurs && !rightRecurs)
			{
				RecursivelyPlanJoinTree(query, joinExpr->rarg, context);

				rightRecurs = true;
			}

			break;
		}

		case JOIN_RIGHT:
		{
			/* recurse into left side if only right side is recurring */
			if (!leftRecurs && rightRecurs)
			{
				RecursivelyPlanJoinTree(query, joinExpr->larg, context);

				leftRecurs = true;
			}

			break;
		}

		case JOIN_FULL:
		{
			/* recurse into right side if only left side is recurring */
			if (leftRecurs && !rightRecurs)
			{
				RecursivelyPlanJoinTree(query, joinExpr->rarg, context);

				rightRecurs = true;
			}

			/* recurse into left side if only right side is recurring */
			if (!leftRecurs && rightRecurs)
			{
				RecursivelyPlanJoinTree(query, joinExpr->rarg, context);

				leftRecurs = true;
			}

			break;
		}

		default:
		case JOIN_INNER:
		{
			/* inner joins with recurring tuples can be safely executed */
			break;
		}
	}

	if (leftRecurs && rightRecurs)
	{
		/* both sides are already recurring, no need to continue */
		return;
	}

	/* prevent recurring outer joins further down the join tree */
	RecursivelyPlanRecurringOuterJoins(query, joinExpr->larg, context);
	RecursivelyPlanRecurringOuterJoins(query, joinExpr->rarg, context);
}


/*
 * JoinTreeContainsDistributedTableRTE returns whether a distributed table RTE
 * appears in the join tree of a query. This is used to determine whether the
 * inner side of an outer join should be recursively planned.
 */
static bool
JoinTreeContainsDistributedTableRTE(Query *query, Node *joinTreeNode)
{
	if (joinTreeNode == NULL)
	{
		return false;
	}

	if (IsA(joinTreeNode, JoinExpr))
	{
		JoinExpr *joinExpr = (JoinExpr *) joinTreeNode;

		if (JoinTreeContainsDistributedTableRTE(query, joinExpr->larg))
		{
			return true;
		}

		if (JoinTreeContainsDistributedTableRTE(query, joinExpr->rarg))
		{
			return true;
		}
	}
	else if (IsA(joinTreeNode, RangeTblRef))
	{
		RangeTblRef *rangeTableRef = (RangeTblRef *) joinTreeNode;
		RangeTblEntry *rangeTableEntry = rt_fetch(rangeTableRef->rtindex, query->rtable);

		if (IsDistributedTableRTE((Node *) rangeTableEntry))
		{
			return true;
		}

		if (rangeTableEntry->rtekind == RTE_SUBQUERY &&
			QueryContainsDistributedTableRTE(rangeTableEntry->subquery))
		{
			return true;
		}
	}

	return false;
}


static bool
QueryContainsDistributedTableRTE(Query *query)
{
	ListCell *rteCell = NULL;

	foreach(rteCell, query->rtable)
	{
		RangeTblEntry *rangeTableEntry = (RangeTblEntry *) lfirst(rteCell);

		if (IsDistributedTableRTE((Node *) rangeTableEntry))
		{
			return true;
		}

		if (rangeTableEntry->rtekind == RTE_SUBQUERY &&
			QueryContainsDistributedTableRTE(rangeTableEntry->subquery))
		{
			return true;
		}
	}

	return false;
}


static bool
IsDistributedTableRTE(Node *node)
{
	RangeTblEntry *rangeTableEntry = NULL;
	Oid relationId = InvalidOid;

	if (!IsA(node, RangeTblEntry))
	{
		return false;
	}

	rangeTableEntry = (RangeTblEntry *) node;
	if (rangeTableEntry->rtekind != RTE_RELATION)
	{
		return false;
	}

	relationId = rangeTableEntry->relid;
	if (!IsDistributedTable(relationId))
	{
		return false;
	}

	if (PartitionMethod(relationId) == DISTRIBUTE_BY_NONE)
	{
		return false;
	}

	return true;
}


static DeferredErrorMessage *
PlanPullPushCTEs(Query *query, PlanPullPushContext *pullPushContext)
{
	ListCell *cteCell = NULL;
	CteReferenceWalkerContext context = { -1, NIL };

	if (query->hasModifyingCTE)
	{
		/* we could easily support these, but it's a little scary */
		return DeferredError(ERRCODE_FEATURE_NOT_SUPPORTED,
							 "data-modifying statements are not supported in "
							 "the WITH clauses of distributed queries",
							 NULL, NULL);
	}

	if (query->hasRecursive)
	{
		return DeferredError(ERRCODE_FEATURE_NOT_SUPPORTED,
							 "recursive CTEs are not supported in distributed "
							 "queries",
							 NULL, NULL);
	}

	/* get all RTE_CTEs that point to CTEs from cteList */
	CteReferenceListWalker((Node *) query, &context);

	foreach(cteCell, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(cteCell);
		char *cteName = cte->ctename;
		Query *subquery = (Query *) cte->ctequery;
		Query *subPlanQuery = copyObject(subquery);
		uint64 planId = pullPushContext->planId;
		int subPlanId = list_length(pullPushContext->subPlanList);
		Query *resultQuery = NULL;
		PlannedStmt *subPlan = NULL;
		ListCell *rteCell = NULL;
		int cursorOptions = 0;

		if (ContainsReferencesToOuterQuery(subquery))
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("CTEs that refer to other subqueries are not "
								   "supported in multi-shard queries")));
		}

		/* build a subplan for the CTE */
		resultQuery = BuildSubPlanResultQuery(subquery, planId, subPlanId);

		if (log_min_messages >= DEBUG1)
		{
			StringInfo subPlanString = makeStringInfo();
			pg_get_query_def(subPlanQuery, subPlanString);
			elog(DEBUG1, "building subplan for query: %s", subPlanString->data);
		}

		/* replace references to the CTE with a subquery that reads results */
		foreach(rteCell, context.cteReferenceList)
		{
			RangeTblEntry *rangeTableEntry = (RangeTblEntry *) lfirst(rteCell);

			if (rangeTableEntry->rtekind != RTE_CTE)
			{
				/* RTE was already replaced and its ctename is NULL */
				continue;
			}

			if (strncmp(rangeTableEntry->ctename, cteName, NAMEDATALEN) == 0)
			{
				if (log_min_messages >= DEBUG1)
				{
					StringInfo resultQueryString = makeStringInfo();
					pg_get_query_def(resultQuery, resultQueryString);
					elog(DEBUG1, "replacing CTE reference %s --> %s",
						 cteName, resultQueryString->data);
				}

				pfree(rangeTableEntry->ctename);

				rangeTableEntry->rtekind = RTE_SUBQUERY;

				/* TODO: can avoid copy the first time */
				rangeTableEntry->subquery = copyObject(resultQuery);
				rangeTableEntry->ctename = NULL;
				rangeTableEntry->ctelevelsup = 0;
			}
		}

		if (ContainsResultFunctionWalker((Node *) subPlanQuery, NULL))
		{
			/*
			 * Make sure we go through distributed planning for a function
			 * with not relation but only read_records_file calls.
			 */
			cursorOptions |= CURSOR_OPT_FORCE_DISTRIBUTED;
		}

		/* we want to be able to handle queries with only intermediate results */
		if (!EnableRouterExecution)
		{
			ereport(ERROR, (errmsg("cannot handle CTEs when the router executor "
								   "is disabled")));
		}

		subPlan = planner(subPlanQuery, cursorOptions, NULL);
		pullPushContext->subPlanList = lappend(pullPushContext->subPlanList, subPlan);
	}

	/*
	 * All CTEs are now executed through subplans and RTE_CTEs pointing
	 * to the CTE list have been replaced with subqueries. We can now
	 * clear the cteList. (TODO: maybe free it?)
	 */
	query->cteList = NIL;

	return NULL;
}


/*
 * CteReferenceList
 */
static bool
CteReferenceListWalker(Node *node, CteReferenceWalkerContext *context)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rangeTableEntry = (RangeTblEntry *) node;

		if (rangeTableEntry->rtekind == RTE_CTE &&
			rangeTableEntry->ctelevelsup == context->level)
		{
			context->cteReferenceList = lappend(context->cteReferenceList,
												rangeTableEntry);
		}

		/* caller will descend into range table entry */
		return false;
	}
	else if (IsA(node, Query))
	{
		Query *query = (Query *) node;

		context->level += 1;
		query_tree_walker(query, CteReferenceListWalker, context, QTW_EXAMINE_RTES);
		context->level -= 1;

		return false;
	}
	else
	{
		return expression_tree_walker(node, CteReferenceListWalker, context);
	}
}


bool
ContainsResultFunction(Node *node)
{
	return ContainsResultFunctionWalker(node, NULL);
}


/*
 * ContainsResultFunctionWalker
 */
static bool
ContainsResultFunctionWalker(Node *node, void *context)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, FuncExpr))
	{
		FuncExpr *funcExpr = (FuncExpr *) node;

		if (funcExpr->funcid == CitusResultFileFuncId())
		{
			return true;
		}

		/* continue into expression_tree_walker */
	}
	else if (IsA(node, Query))
	{
		return query_tree_walker((Query *) node, ContainsResultFunctionWalker, context,
								 0);
	}

	return expression_tree_walker(node, ContainsResultFunctionWalker, context);
}


static bool
PlanPullPushSubqueriesWalker(Node *node, PlanPullPushContext *context)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;

		context->level += 1;
		PlanPullPushSubqueries(query, context);
		context->level -= 1;

		if (ShouldRecursivelyPlanSubquery(query, context))
		{
			uint64 planId = context->planId;
			int subPlanId = list_length(context->subPlanList);

			PlannedStmt *subPlan = RecursivelyPlanQuery(query, planId, subPlanId);
			context->subPlanList = lappend(context->subPlanList, subPlan);
		}
		else
		{
			PlannerRestrictionContext *filteredPlannerRestriction =
				FilterPlannerRestrictionForQuery((*context).plannerRestrictionContext,
												 query);

			/*
			 * We might still want to check whether the query contains colocated joins.
			 * If not, replace the required ones here.
			 */
			if (!ContainsUnionSubquery(query) &&
				   !RestrictionEquivalenceForPartitionKeys(filteredPlannerRestriction))
			{
				int rteMin = FindRTEIdentityWithLeastColocatedJoins(
						filteredPlannerRestriction);

				Relids queryRteIdentities = QueryRteIdentities(query);

				if (bms_is_member(rteMin, queryRteIdentities))
				{
					int subPlanId = list_length(context->subPlanList);
					PlannedStmt *subPlan = RecursivelyPlanQuery(query, context->planId, subPlanId);

					context->subPlanList = lappend(context->subPlanList,
														   subPlan);
				}
			}
		}

		return false;
	}
	else
	{
		return expression_tree_walker(node, PlanPullPushSubqueriesWalker, context);
	}
}


/*
 * RecursivelyPlanQuery recursively plans a query, replaces it with a
 * result query and returns the subplan.
 */
static PlannedStmt *
RecursivelyPlanQuery(Query *query, uint64 planId, int subPlanId)
{
	PlannedStmt *subPlan = NULL;
	Query *resultQuery = NULL;
	int cursorOptions = 0;

	resultQuery = BuildSubPlanResultQuery(query, planId, subPlanId);

	if (log_min_messages >= DEBUG1)
	{
		StringInfo subqueryString = makeStringInfo();
		StringInfo resultQueryString = makeStringInfo();

		pg_get_query_def(query, subqueryString);
		pg_get_query_def(resultQuery, resultQueryString);

		elog(DEBUG1, "replacing subquery %s --> %s", subqueryString->data,
			 resultQueryString->data);
	}

	if (ContainsResultFunction((Node *) query))
	{
		cursorOptions |= CURSOR_OPT_FORCE_DISTRIBUTED;
	}

	/* we want to be able to handle queries with only intermediate results */
	if (!EnableRouterExecution)
	{
		ereport(ERROR, (errmsg("cannot handle complex subqueries when the "
							   "router executor is disabled")));
	}

	subPlan = planner(copyObject(query), cursorOptions, NULL);

	memcpy(query, resultQuery, sizeof(Query));

	return subPlan;
}


/*
 *
 */
static bool
ShouldRecursivelyPlanSubquery(Query *query, PlanPullPushContext *context)
{
	bool shouldRecursivelyPlan = false;
	DeferredErrorMessage *pushdownError = NULL;

	if (ContainsReferencesToOuterQuery(query))
	{
		/* cannot plan correlated subqueries by themselves */

		if (log_min_messages >= DEBUG1)
		{
			/* we cannot deparse queries with references to outer queries */
			elog(DEBUG1, "query includes reference to outer queries, "
						 "so not being recursively planned");
		}

		return false;
	}

	pushdownError = DeferErrorIfCannotPushdownSubquery(query, false);
	if (pushdownError != NULL)
	{
		if (!NeedsDistributedPlanning(query))
		{
			/* postgres can always plan queries that don't require distributed planning */
			shouldRecursivelyPlan = true;
		}
		else if (TaskExecutorType == MULTI_EXECUTOR_TASK_TRACKER &&
				 SingleRelationRepartitionSubquery(query))
		{
			/* we could plan this subquery through re-partitioning */
		}
		else if (false)
		{
			/*
			 * TODO: At this point, we should check one more thing:
			 *
			 * If we've replaced all FROM subqueries, we should somehome
			 * recurse into the sublink. Example query:
			 *
			 * SELECT
					foo.user_id
				FROM
					(SELECT users_table.user_id, event_type FROM users_table, events_table WHERE users_table.user_id = events_table.value_2 AND event_type IN (1,2,3,4)) as foo,
					(SELECT users_table.user_id FROM users_table, events_table WHERE users_table.user_id = events_table.event_type AND event_type IN (5,6,7,8)) as bar
				WHERE
					foo.user_id = bar.user_id AND
					foo.event_type IN (SELECT event_type FROM events_table WHERE user_id < 100);
			 *
			 *
			 */
		}
		else
		{
			DeferredErrorMessage *unsupportedQueryError = NULL;

			unsupportedQueryError = DeferErrorIfQueryNotSupported(query);
			if (unsupportedQueryError == NULL)
			{
				/* Citus can plan this distribute (sub)query */
				shouldRecursivelyPlan = true;
			}
		}
	}
	else
	{
		PlannerRestrictionContext *filteredRestrictionContext =
			FilterPlannerRestrictionForQuery(context->plannerRestrictionContext, query);

		if (!ContainsUnionSubquery(query) &&
			DeferErrorIfQueryNotSupported(query) == NULL &&
			SubqueryEntryList(query) == NIL &&
			!RestrictionEquivalenceForPartitionKeys(filteredRestrictionContext))
		{
			shouldRecursivelyPlan = true;
		}
	}

	return shouldRecursivelyPlan;
}


/*
 * ContainsReferencesToOuterQuery
 */
static bool
ContainsReferencesToOuterQuery(Query *query)
{
	VarLevelsUpWalkerContext context = { 0 };
	int flags = 0;

	return query_tree_walker(query, ContainsReferencesToOuterQueryWalker,
							 &context, flags);
}


/*
 * ContainsReferencesToOuterQueryWalker
 */
static bool
ContainsReferencesToOuterQueryWalker(Node *node, VarLevelsUpWalkerContext *context)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup > context->level)
		{
			return true;
		}

		return false;
	}

	if (IsA(node, CurrentOfExpr))
	{
		return true;
	}

	if (IsA(node, PlaceHolderVar))
	{
		if (((PlaceHolderVar *) node)->phlevelsup > context->level)
		{
			return true;
		}
	}

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;
		bool found = false;
		int flags = 0;

		context->level += 1;
		found = query_tree_walker(query, ContainsReferencesToOuterQueryWalker,
								  context, flags);
		context->level -= 1;

		return found;
	}
	else
	{
		return expression_tree_walker(node, ContainsReferencesToOuterQueryWalker,
									  context);
	}
}


static Query *
BuildSubPlanResultQuery(Query *subquery, uint64 planId, int subPlanId)
{
	Query *resultQuery = NULL;
	StringInfo resultFileName = makeStringInfo();
	Const *resultFileNameConst = NULL;
	Const *resultFormatConst = NULL;
	FuncExpr *funcExpr = NULL;
	Alias *funcAlias = NULL;
	List *funcColNames = NIL;
	List *funcColTypes = NIL;
	List *funcColTypMods = NIL;
	List *funcColCollations = NIL;
	RangeTblFunction *rangeTableFunction = NULL;
	RangeTblEntry *rangeTableEntry = NULL;
	RangeTblRef *rangeTableRef = NULL;
	FromExpr *joinTree = NULL;
	ListCell *targetEntryCell = NULL;
	List *targetList = NIL;
	int columnNumber = 1;

	foreach(targetEntryCell, subquery->targetList)
	{
		TargetEntry *targetEntry = (TargetEntry *) lfirst(targetEntryCell);
		Node *targetExpr = (Node *) targetEntry->expr;
		char *columnName = targetEntry->resname;
		Oid columnType = exprType(targetExpr);
		Oid columnTypMod = exprTypmod(targetExpr);
		Oid columnCollation = exprCollation(targetExpr);
		Var *functionColumnVar = NULL;
		TargetEntry *newTargetEntry = makeNode(TargetEntry);

		if (targetEntry->resjunk)
		{
			continue;
		}

		funcColNames = lappend(funcColNames, makeString(columnName));
		funcColTypes = lappend_int(funcColTypes, columnType);
		funcColTypMods = lappend_int(funcColTypMods, columnTypMod);
		funcColCollations = lappend_int(funcColCollations, columnCollation);

		functionColumnVar = makeNode(Var);
		functionColumnVar->varno = 1;
		functionColumnVar->varattno = columnNumber;
		functionColumnVar->vartype = columnType;
		functionColumnVar->vartypmod = columnTypMod;
		functionColumnVar->varcollid = columnCollation;
		functionColumnVar->varlevelsup = 0;
		functionColumnVar->varnoold = 1;
		functionColumnVar->varoattno = columnNumber;
		functionColumnVar->location = -1;

		newTargetEntry = makeNode(TargetEntry);
		newTargetEntry->expr = (Expr *) functionColumnVar;
		newTargetEntry->resno = columnNumber;
		newTargetEntry->resname = columnName;

		targetList = lappend(targetList, newTargetEntry);

		columnNumber++;
	}

	appendStringInfo(resultFileName,
					 "base/pgsql_job_cache/%d_%d_%d_" UINT64_FORMAT "_%d.data",
					 GetUserId(), GetLocalGroupId(), MyProcPid, planId, subPlanId);

	resultFileNameConst = makeNode(Const);
	resultFileNameConst->consttype = TEXTOID;
	resultFileNameConst->consttypmod = -1;
	resultFileNameConst->constlen = -1;
	resultFileNameConst->constvalue = CStringGetTextDatum(resultFileName->data);
	resultFileNameConst->constbyval = false;
	resultFileNameConst->constisnull = false;
	resultFileNameConst->location = -1;

	resultFormatConst = makeNode(Const);
	resultFormatConst->consttype = TEXTOID;
	resultFormatConst->consttypmod = -1;
	resultFormatConst->constlen = -1;
	resultFormatConst->constvalue = CStringGetTextDatum("binary");
	resultFormatConst->constbyval = false;
	resultFormatConst->constisnull = false;
	resultFormatConst->location = -1;

	funcExpr = makeNode(FuncExpr);
	funcExpr->funcid = CitusResultFileFuncId();
	funcExpr->funcretset = true;
	funcExpr->funcvariadic = false;
	funcExpr->funcformat = 0;
	funcExpr->funccollid = 0;
	funcExpr->inputcollid = 100; /* TODO, what's this value? */
	funcExpr->location = -1; /* TODO 68 */
	funcExpr->args = list_make2(resultFileNameConst, resultFormatConst);

	rangeTableFunction = makeNode(RangeTblFunction);
	rangeTableFunction->funccolcount = list_length(funcColNames);
	rangeTableFunction->funccolnames = funcColNames;
	rangeTableFunction->funccoltypes = funcColTypes;
	rangeTableFunction->funccoltypmods = funcColTypMods;
	rangeTableFunction->funccolcollations = funcColCollations;
	rangeTableFunction->funcparams = NULL;
	rangeTableFunction->funcexpr = (Node *) funcExpr;

	funcAlias = makeNode(Alias);
	funcAlias->aliasname = "read_records_file";
	funcAlias->colnames = funcColNames;

	rangeTableEntry = makeNode(RangeTblEntry);
	rangeTableEntry->rtekind = RTE_FUNCTION;
	rangeTableEntry->functions = list_make1(rangeTableFunction);
	rangeTableEntry->inFromCl = true;
	rangeTableEntry->eref = funcAlias;

	rangeTableRef = makeNode(RangeTblRef);
	rangeTableRef->rtindex = 1;

	joinTree = makeNode(FromExpr);
	joinTree->fromlist = list_make1(rangeTableRef);

	resultQuery = makeNode(Query);
	resultQuery->commandType = CMD_SELECT;
	resultQuery->rtable = list_make1(rangeTableEntry);
	resultQuery->jointree = joinTree;
	resultQuery->targetList = targetList;

	return resultQuery;
}


/*
 * GetDistributedPlan returns the associated DistributedPlan for a CustomScan.
 */
DistributedPlan *
GetDistributedPlan(CustomScan *customScan)
{
	Node *node = NULL;
	DistributedPlan *distributedPlan = NULL;

	Assert(list_length(customScan->custom_private) == 1);

	node = (Node *) linitial(customScan->custom_private);
	Assert(CitusIsA(node, DistributedPlan));

	node = CheckNodeCopyAndSerialization(node);

	/*
	 * When using prepared statements the same plan gets reused across
	 * multiple statements and transactions. We make several modifications
	 * to the DistributedPlan during execution such as assigning task placements
	 * and evaluating functions and parameters. These changes should not
	 * persist, so we always work on a copy.
	 */
	distributedPlan = (DistributedPlan *) copyObject(node);

	return distributedPlan;
}


/*
 * FinalizePlan combines local plan with distributed plan and creates a plan
 * which can be run by the PostgreSQL executor.
 */
static PlannedStmt *
FinalizePlan(PlannedStmt *localPlan, DistributedPlan *distributedPlan)
{
	PlannedStmt *finalPlan = NULL;
	CustomScan *customScan = makeNode(CustomScan);
	Node *distributedPlanData = NULL;
	MultiExecutorType executorType = MULTI_EXECUTOR_INVALID_FIRST;

	if (!distributedPlan->planningError)
	{
		executorType = JobExecutorType(distributedPlan);
	}

	switch (executorType)
	{
		case MULTI_EXECUTOR_REAL_TIME:
		{
			customScan->methods = &RealTimeCustomScanMethods;
			break;
		}

		case MULTI_EXECUTOR_TASK_TRACKER:
		{
			customScan->methods = &TaskTrackerCustomScanMethods;
			break;
		}

		case MULTI_EXECUTOR_ROUTER:
		{
			customScan->methods = &RouterCustomScanMethods;
			break;
		}

		case MULTI_EXECUTOR_COORDINATOR_INSERT_SELECT:
		{
			customScan->methods = &CoordinatorInsertSelectCustomScanMethods;
			break;
		}

		default:
		{
			customScan->methods = &DelayedErrorCustomScanMethods;
			break;
		}
	}

	if (IsMultiTaskPlan(distributedPlan))
	{
		/* if it is not a single task executable plan, inform user according to the log level */
		if (MultiTaskQueryLogLevel != MULTI_TASK_QUERY_INFO_OFF)
		{
			ereport(MultiTaskQueryLogLevel, (errmsg(
												 "multi-task query about to be executed"),
											 errhint(
												 "Queries are split to multiple tasks "
												 "if they have to be split into several"
												 " queries on the workers.")));
		}
	}

	distributedPlan->relationIdList = localPlan->relationOids;

	distributedPlanData = (Node *) distributedPlan;

	customScan->custom_private = list_make1(distributedPlanData);
	customScan->flags = CUSTOMPATH_SUPPORT_BACKWARD_SCAN;

	if (distributedPlan->masterQuery)
	{
		finalPlan = FinalizeNonRouterPlan(localPlan, distributedPlan, customScan);
	}
	else
	{
		finalPlan = FinalizeRouterPlan(localPlan, customScan);
	}

	return finalPlan;
}


/*
 * FinalizeNonRouterPlan gets the distributed custom scan plan, and creates the
 * final master select plan on the top of this distributed plan for real-time
 * and task-tracker executors.
 */
static PlannedStmt *
FinalizeNonRouterPlan(PlannedStmt *localPlan, DistributedPlan *distributedPlan,
					  CustomScan *customScan)
{
	PlannedStmt *finalPlan = NULL;

	finalPlan = MasterNodeSelectPlan(distributedPlan, customScan);
	finalPlan->queryId = localPlan->queryId;
	finalPlan->utilityStmt = localPlan->utilityStmt;

	/* add original range table list for access permission checks */
	finalPlan->rtable = list_concat(finalPlan->rtable, localPlan->rtable);

	return finalPlan;
}


/*
 * FinalizeRouterPlan gets a CustomScan node which already wrapped distributed
 * part of a router plan and sets it as the direct child of the router plan
 * because we don't run any query on master node for router executable queries.
 * Here, we also rebuild the column list to read from the remote scan.
 */
static PlannedStmt *
FinalizeRouterPlan(PlannedStmt *localPlan, CustomScan *customScan)
{
	PlannedStmt *routerPlan = NULL;
	RangeTblEntry *remoteScanRangeTableEntry = NULL;
	ListCell *targetEntryCell = NULL;
	List *targetList = NIL;
	List *columnNameList = NIL;

	/* we will have custom scan range table entry as the first one in the list */
	int customScanRangeTableIndex = 1;

	/* build a targetlist to read from the custom scan output */
	foreach(targetEntryCell, localPlan->planTree->targetlist)
	{
		TargetEntry *targetEntry = lfirst(targetEntryCell);
		TargetEntry *newTargetEntry = NULL;
		Var *newVar = NULL;
		Value *columnName = NULL;

		Assert(IsA(targetEntry, TargetEntry));

		/*
		 * This is unlikely to be hit because we would not need resjunk stuff
		 * at the toplevel of a router query - all things needing it have been
		 * pushed down.
		 */
		if (targetEntry->resjunk)
		{
			continue;
		}

		/* build target entry pointing to remote scan range table entry */
		newVar = makeVarFromTargetEntry(customScanRangeTableIndex, targetEntry);
		newTargetEntry = flatCopyTargetEntry(targetEntry);
		newTargetEntry->expr = (Expr *) newVar;
		targetList = lappend(targetList, newTargetEntry);

		columnName = makeString(targetEntry->resname);
		columnNameList = lappend(columnNameList, columnName);
	}

	customScan->scan.plan.targetlist = targetList;

	routerPlan = makeNode(PlannedStmt);
	routerPlan->planTree = (Plan *) customScan;

	remoteScanRangeTableEntry = RemoteScanRangeTableEntry(columnNameList);
	routerPlan->rtable = list_make1(remoteScanRangeTableEntry);

	/* add original range table list for access permission checks */
	routerPlan->rtable = list_concat(routerPlan->rtable, localPlan->rtable);

	routerPlan->canSetTag = true;
	routerPlan->relationOids = NIL;

	routerPlan->queryId = localPlan->queryId;
	routerPlan->utilityStmt = localPlan->utilityStmt;
	routerPlan->commandType = localPlan->commandType;
	routerPlan->hasReturning = localPlan->hasReturning;

	return routerPlan;
}


/*
 * RemoteScanRangeTableEntry creates a range table entry from given column name
 * list to represent a remote scan.
 */
RangeTblEntry *
RemoteScanRangeTableEntry(List *columnNameList)
{
	RangeTblEntry *remoteScanRangeTableEntry = makeNode(RangeTblEntry);

	/* we use RTE_VALUES for custom scan because we can't look up relation */
	remoteScanRangeTableEntry->rtekind = RTE_VALUES;
	remoteScanRangeTableEntry->eref = makeAlias("remote_scan", columnNameList);
	remoteScanRangeTableEntry->inh = false;
	remoteScanRangeTableEntry->inFromCl = true;

	return remoteScanRangeTableEntry;
}


/*
 * CheckNodeIsDumpable checks that the passed node can be dumped using
 * nodeToString(). As this checks is expensive, it's only active when
 * assertions are enabled.
 */
static void
CheckNodeIsDumpable(Node *node)
{
#ifdef USE_ASSERT_CHECKING
	char *out = nodeToString(node);
	pfree(out);
#endif
}


/*
 * CheckNodeCopyAndSerialization checks copy/dump/read functions
 * for nodes and returns copy of the input.
 *
 * It is only active when assertions are enabled, otherwise it returns
 * the input directly. We use this to confirm that our serialization
 * and copy logic produces the correct plan during regression tests.
 *
 * It does not check string equality on node dumps due to differences
 * in some Postgres types.
 */
static Node *
CheckNodeCopyAndSerialization(Node *node)
{
#ifdef USE_ASSERT_CHECKING
	char *out = nodeToString(node);
	Node *deserializedNode = (Node *) stringToNode(out);
	Node *nodeCopy = copyObject(deserializedNode);
	char *outCopy = nodeToString(nodeCopy);

	pfree(out);
	pfree(outCopy);

	return nodeCopy;
#else
	return node;
#endif
}


/*
 * multi_join_restriction_hook is a hook called by postgresql standard planner
 * to notify us about various planning information regarding joins. We use
 * it to learn about the joining column.
 */
void
multi_join_restriction_hook(PlannerInfo *root,
							RelOptInfo *joinrel,
							RelOptInfo *outerrel,
							RelOptInfo *innerrel,
							JoinType jointype,
							JoinPathExtraData *extra)
{
	PlannerRestrictionContext *plannerRestrictionContext = NULL;
	JoinRestrictionContext *joinRestrictionContext = NULL;
	JoinRestriction *joinRestriction = NULL;
	MemoryContext restrictionsMemoryContext = NULL;
	MemoryContext oldMemoryContext = NULL;
	List *restrictInfoList = NIL;

	/*
	 * Use a memory context that's guaranteed to live long enough, could be
	 * called in a more shorted lived one (e.g. with GEQO).
	 */
	plannerRestrictionContext = CurrentPlannerRestrictionContext();
	restrictionsMemoryContext = plannerRestrictionContext->memoryContext;
	oldMemoryContext = MemoryContextSwitchTo(restrictionsMemoryContext);

	/*
	 * We create a copy of restrictInfoList because it may be created in a memory
	 * context which will be deleted when we still need it, thus we create a copy
	 * of it in our memory context.
	 */
	restrictInfoList = copyObject(extra->restrictlist);

	joinRestrictionContext = plannerRestrictionContext->joinRestrictionContext;
	Assert(joinRestrictionContext != NULL);

	joinRestriction = palloc0(sizeof(JoinRestriction));
	joinRestriction->joinType = jointype;
	joinRestriction->joinRestrictInfoList = restrictInfoList;
	joinRestriction->plannerInfo = root;
	joinRestriction->innerrel = innerrel;
	joinRestriction->outerrel = outerrel;

	joinRestrictionContext->joinRestrictionList =
		lappend(joinRestrictionContext->joinRestrictionList, joinRestriction);

	MemoryContextSwitchTo(oldMemoryContext);
}


/*
 * multi_relation_restriction_hook is a hook called by postgresql standard planner
 * to notify us about various planning information regarding a relation. We use
 * it to retrieve restrictions on relations.
 */
void
multi_relation_restriction_hook(PlannerInfo *root, RelOptInfo *relOptInfo, Index index,
								RangeTblEntry *rte)
{
	PlannerRestrictionContext *plannerRestrictionContext = NULL;
	RelationRestrictionContext *relationRestrictionContext = NULL;
	MemoryContext restrictionsMemoryContext = NULL;
	MemoryContext oldMemoryContext = NULL;
	RelationRestriction *relationRestriction = NULL;
	DistTableCacheEntry *cacheEntry = NULL;
	bool distributedTable = false;
	bool localTable = false;

	if (rte->rtekind != RTE_RELATION)
	{
		return;
	}

	/*
	 * Use a memory context that's guaranteed to live long enough, could be
	 * called in a more shorted lived one (e.g. with GEQO).
	 */
	plannerRestrictionContext = CurrentPlannerRestrictionContext();
	restrictionsMemoryContext = plannerRestrictionContext->memoryContext;
	oldMemoryContext = MemoryContextSwitchTo(restrictionsMemoryContext);

	distributedTable = IsDistributedTable(rte->relid);
	localTable = !distributedTable;

	relationRestriction = palloc0(sizeof(RelationRestriction));
	relationRestriction->index = index;
	relationRestriction->relationId = rte->relid;
	relationRestriction->rte = rte;
	relationRestriction->relOptInfo = relOptInfo;
	relationRestriction->distributedRelation = distributedTable;
	relationRestriction->plannerInfo = root;
	relationRestriction->parentPlannerInfo = root->parent_root;
	relationRestriction->prunedShardIntervalList = NIL;

	/* see comments on GetVarFromAssignedParam() */
	if (relationRestriction->parentPlannerInfo)
	{
		relationRestriction->parentPlannerParamList =
			CopyPlanParamList(root->parent_root->plan_params);
	}

	relationRestrictionContext = plannerRestrictionContext->relationRestrictionContext;
	relationRestrictionContext->hasDistributedRelation |= distributedTable;
	relationRestrictionContext->hasLocalRelation |= localTable;

	/*
	 * We're also keeping track of whether all participant
	 * tables are reference tables.
	 */
	if (distributedTable)
	{
		cacheEntry = DistributedTableCacheEntry(rte->relid);

		relationRestrictionContext->allReferenceTables &=
			(cacheEntry->partitionMethod == DISTRIBUTE_BY_NONE);
	}

	relationRestrictionContext->relationRestrictionList =
		lappend(relationRestrictionContext->relationRestrictionList, relationRestriction);

	MemoryContextSwitchTo(oldMemoryContext);
}


/*
 * CopyPlanParamList deep copies the input PlannerParamItem list and returns the newly
 * allocated list.
 * Note that we cannot use copyObject() function directly since there is no support for
 * copying PlannerParamItem structs.
 */
static List *
CopyPlanParamList(List *originalPlanParamList)
{
	ListCell *planParamCell = NULL;
	List *copiedPlanParamList = NIL;

	foreach(planParamCell, originalPlanParamList)
	{
		PlannerParamItem *originalParamItem = lfirst(planParamCell);
		PlannerParamItem *copiedParamItem = makeNode(PlannerParamItem);

		copiedParamItem->paramId = originalParamItem->paramId;
		copiedParamItem->item = copyObject(originalParamItem->item);

		copiedPlanParamList = lappend(copiedPlanParamList, copiedParamItem);
	}

	return copiedPlanParamList;
}


/*
 * CreateAndPushPlannerRestrictionContext creates a new relation restriction context
 * and a new join context, inserts it to the beginning of the
 * plannerRestrictionContextList. Finally, the planner restriction context is
 * inserted to the beginning of the plannerRestrictionContextList and it is returned.
 */
static PlannerRestrictionContext *
CreateAndPushPlannerRestrictionContext(void)
{
	PlannerRestrictionContext *plannerRestrictionContext =
		palloc0(sizeof(PlannerRestrictionContext));

	plannerRestrictionContext->relationRestrictionContext =
		palloc0(sizeof(RelationRestrictionContext));

	plannerRestrictionContext->joinRestrictionContext =
		palloc0(sizeof(JoinRestrictionContext));

	plannerRestrictionContext->memoryContext = CurrentMemoryContext;

	/* we'll apply logical AND as we add tables */
	plannerRestrictionContext->relationRestrictionContext->allReferenceTables = true;

	plannerRestrictionContextList = lcons(plannerRestrictionContext,
										  plannerRestrictionContextList);

	return plannerRestrictionContext;
}


/*
 * CurrentRestrictionContext returns the the most recently added
 * PlannerRestrictionContext from the plannerRestrictionContextList list.
 */
static PlannerRestrictionContext *
CurrentPlannerRestrictionContext(void)
{
	PlannerRestrictionContext *plannerRestrictionContext = NULL;

	Assert(plannerRestrictionContextList != NIL);

	plannerRestrictionContext =
		(PlannerRestrictionContext *) linitial(plannerRestrictionContextList);

	return plannerRestrictionContext;
}


/*
 * PopPlannerRestrictionContext removes the most recently added restriction contexts from
 * the planner restriction context list. The function assumes the list is not empty.
 */
static void
PopPlannerRestrictionContext(void)
{
	plannerRestrictionContextList = list_delete_first(plannerRestrictionContextList);
}


/*
 * HasUnresolvedExternParamsWalker returns true if the passed in expression
 * has external parameters that are not contained in boundParams, false
 * otherwise.
 */
static bool
HasUnresolvedExternParamsWalker(Node *expression, ParamListInfo boundParams)
{
	if (expression == NULL)
	{
		return false;
	}

	if (IsA(expression, Param))
	{
		Param *param = (Param *) expression;
		int paramId = param->paramid;

		/* only care about user supplied parameters */
		if (param->paramkind != PARAM_EXTERN)
		{
			return false;
		}

		/* check whether parameter is available (and valid) */
		if (boundParams && paramId > 0 && paramId <= boundParams->numParams)
		{
			ParamExternData *externParam = &boundParams->params[paramId - 1];

			/* give hook a chance in case parameter is dynamic */
			if (!OidIsValid(externParam->ptype) && boundParams->paramFetch != NULL)
			{
				(*boundParams->paramFetch)(boundParams, paramId);
			}

			if (OidIsValid(externParam->ptype))
			{
				return false;
			}
		}

		return true;
	}

	/* keep traversing */
	if (IsA(expression, Query))
	{
		return query_tree_walker((Query *) expression,
								 HasUnresolvedExternParamsWalker,
								 boundParams,
								 0);
	}
	else
	{
		return expression_tree_walker(expression,
									  HasUnresolvedExternParamsWalker,
									  boundParams);
	}
}
