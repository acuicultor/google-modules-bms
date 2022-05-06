/* Copyright (c) 2015-2016 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __PMIC_VOTER_H
#define __PMIC_VOTER_H

struct votable;

enum votable_type {
	VOTE_MIN,
	VOTE_MAX,
	VOTE_SET_ANY,
	NUM_VOTABLE_TYPES,
};

bool __deprecated is_client_vote_enabled(struct votable *votable, const char *client_str);
bool __deprecated is_client_vote_enabled_locked(struct votable *votable,
							const char *client_str);
int __deprecated get_client_vote(struct votable *votable, const char *client_str);
int __deprecated get_client_vote_locked(struct votable *votable, const char *client_str);
int __deprecated get_effective_result(struct votable *votable);
int __deprecated get_effective_result_locked(struct votable *votable);
const char * __deprecated get_effective_client(struct votable *votable);
const char * __deprecated get_effective_client_locked(struct votable *votable);
int __deprecated vote(struct votable *votable, const char *client_str, bool state, int val);
int __deprecated rerun_election(struct votable *votable);
struct votable * __deprecated find_votable(const char *name);
struct votable * __deprecated create_votable(const char *name,
				int votable_type,
				int (*callback)(struct votable *votable,
						void *data,
						int effective_result,
						const char *effective_client),
				void *data);
void __deprecated destroy_votable(struct votable *votable);
void __deprecated lock_votable(struct votable *votable);
void __deprecated unlock_votable(struct votable *votable);

#endif /* __PMIC_VOTER_H */
