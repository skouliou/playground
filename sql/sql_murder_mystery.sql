-- get the crime transcript
SELECT description
FROM crime_scene_report
WHERE date = 20180115
AND type = 'murder'
AND city = 'SQL City';


-- get the id of the two witnesses
SELECT * FROM
(	SELECT * FROM person
	WHERE address_street_name = 'Northwestern Dr'
  	order by address_number DESC
  	limit 1
) AS witness1
  
UNION

SELECT * FROM
(  	SELECT *
	FROM person
	WHERE name LIKE '%Annabel%'
	AND address_street_name = 'Franklin Ave'
) AS witness2;


-- get teh transcript of the two witnesses FROM the interview
SELECT transcript FROM interview
WHERE person_id IN (
  SELECT id FROM
  (	  SELECT * FROM person
	  WHERE address_street_name = 'Northwestern Dr'
	  ORDER BY address_number DESC
	  LIMIT 1
  )

  UNION ALL

  SELECT id FROM
  (   SELECT *
	  FROM person
	  WHERE name LIKE '%Annabel%'
	  AND address_street_name = 'Franklin Ave'
  )
);


-- get the names and transripts of suspects
SELECT person.name, interview.transcript FROM interview
LEFT JOIN person on interview.person_id = person.id
WHERE interview.person_id IN
(
  SELECT person.id
  FROM person LEFT JOIN drivers_license AS license
  WHERE license.plate_number LIKE '%H42W%'
  AND person.id IN (
	SELECT person_id 
	FROM get_fit_now_member AS membership
	LEFT JOIN get_fit_now_check_in AS check_in
	ON membership.id = check_in.membership_id
	WHERE check_in.check_in_date = 20180109
	AND membership.membership_status = 'gold'
	AND membership.id LIKE '48Z%'
  )
);


-- bonus:
SELECT person.name FROM person
LEFT JOIN drivers_license AS license
ON person.license_id = license.id
LEFT JOIN facebook_event_checkin AS event
ON person.id = event.person_id
WHERE license.height BETWEEN 65 AND 67
AND license.hair_color = 'red'
AND license.car_model = 'Model S'
AND event.event_name = 'SQL Symphony Concert'
GROUP BY person.name;
