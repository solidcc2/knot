/*  Copyright (C) 2023 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "knot/conf/conf.h"

#define SERIAL_MAX_INCREMENT 2147483647

/*!
 * \brief result of serial comparison. LOWER means that the first serial is lower that the second.
 *
 * Example: (serial_compare(a, b) & SERIAL_MASK_LEQ) means "a <= b".
 */
typedef enum {
	SERIAL_INCOMPARABLE = 0x0,
	SERIAL_LOWER = 0x1,
	SERIAL_GREATER = 0x2,
	SERIAL_EQUAL = 0x3,
	SERIAL_MASK_LEQ = SERIAL_LOWER,
	SERIAL_MASK_GEQ = SERIAL_GREATER,
} serial_cmp_result_t;

/*!
 * \brief Compares two zone serials.
 */
serial_cmp_result_t serial_compare(uint32_t s1, uint32_t s2);

inline static bool serial_equal(uint32_t a, uint32_t b)
{
	return serial_compare(a, b) == SERIAL_EQUAL;
}

/*!
 * \brief Get (next) serial for given serial update policy.
 *
 * \param current  Current SOA serial.
 * \param policy   Specific policy to use instead of configured one.
 * \param must_increment The minimum difference to the current value.
 *                 0 only ensures policy; 1 also increments.
 * \param rem      Requested remainder after division by the modulus.
 * \param mod      Modulus of the given congruency.
 *
 * \return New serial.
 */
uint32_t serial_next_generic(uint32_t current, unsigned policy, uint32_t must_increment,
                             uint8_t rem, uint8_t mod);

/*!
 * \brief Get (next) serial for given serial update policy.
 *
 * This function is similar to serial_next_generic() but policy and parameters
 * of the congruency are taken from the server configuration.
 *
 * \param current  Current SOA serial.
 * \param conf     Configuration to get serial-policy from.
 * \param zone     Zone to read out configured policy of.
 * \param policy   Specific policy to use instead of configured one.
 * \param must_increment The minimum difference to the current value.
 *                 0 only ensures policy; 1 also increments.
 *
 * \return New serial.
 */
uint32_t serial_next(uint32_t current, conf_t *conf, const knot_dname_t *zone,
                     unsigned policy, uint32_t must_increment);

typedef struct {
	uint32_t serial;
	bool valid;
} kserial_t;

/*!
 * \brief Compares two kserials.
 *
 * If any of them is invalid, they are INCOMPARABLE.
 */
serial_cmp_result_t kserial_cmp(kserial_t a, kserial_t b);

inline static bool kserial_equal(kserial_t a, kserial_t b)
{
	return kserial_cmp(a, b) == SERIAL_EQUAL;
}
